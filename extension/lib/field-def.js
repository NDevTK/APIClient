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

/* THE RECORD. Every name the Send panel reads is here, with the value that MEANS "the producer observed
   nothing of this kind". `name`, `type`, `label` and `required` have no absent value: a field with no wire
   key, no type, no cardinality or no requiredness is not a field, and a producer that cannot state one has
   nothing to render — so they are stated, never defaulted, and `makeFieldDef` asserts each. */
const FIELD_DEF_ABSENT = Object.freeze({
  // ── identity ──
  displayName: null,        // an alias to render INSTEAD of `name` (the GraphQL variables tree writes one);
                            // null = render the wire name itself.
  customName: false,        // the reviewer renamed this field in a previous session.
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
  _astValidValues: null,    // values the bundle was observed setting this field to; null = none observed.
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
         typeof fd.isNumberGuessed === "boolean" && typeof fd._detectedEnum === "boolean",
         "a FieldDef's required/customName/isNumberGuessed/_detectedEnum is not a boolean (" + where + ") — " +
         "each is a claim the panel renders as a badge, and a non-boolean renders whichever way truthiness " +
         "happens to fall");
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
         _fdNullOrList(fd._excludedValues) && _fdNullOrList(fd._astValidValues),
         "a FieldDef carries a list-or-nothing in a third form (" + where + ") — `children: null` means NOT " +
         "a message and `children: []` means a message with no fields, and a consumer that cannot tell them " +
         "apart renders one as the other");
  DCHECK(_fdNullOr(fd._defaultConfidence, "number") && _fdNullOr(fd._requiredConfidence, "number"),
         "a FieldDef's confidence is neither a number nor null (" + where + ") — the panel renders it as a " +
         "percentage, and a non-number renders as NaN%");
  DCHECK((fd._range === null || (typeof fd._range === "object" && !Array.isArray(fd._range))) &&
         (fd._bounds === null || (typeof fd._bounds === "object" && !Array.isArray(fd._bounds))),
         "a FieldDef's `_range`/`_bounds` is neither a record nor null (" + where + ") — both are DOMAINS " +
         "the panel renders as constraints beside the input, and a scalar there would render as a value the " +
         "code computed, which is the one thing §@H forbids a domain from becoming");
  return fd;
}
