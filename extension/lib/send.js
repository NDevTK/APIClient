// lib/send.js — Send-panel replay backend: resolve an endpoint's request schema, coerce the form field values
// to their declared types, and SEND the request from this zone as the frame it was observed in
// (`operatorSendAsFrame`: the frame's Origin, Referer and cookies, through lib/safe-fetch.js's operator tier).
// Extracted from the offscreen-brain.js monolith (one problem per file); loaded before it, resolves the encode
// fns (lib/encode.js) at call-time. The manual testing workbench.

/* A PROBE FIELD IS A DIFFERENT RECORD FROM A FieldDef, AND IT IS TRANSLATED RATHER THAN SPREAD. lib/
   req2proto.js composes it by reading a `google.rpc.Status` the SERVER wrote, and it carries names the Send
   panel has never rendered (`wellKnown`, `package`, `messageName`, `requiredChildren`, `isEnum`) beside the
   ones it has — so `{...probeField}` would smuggle a second vocabulary into the panel's record, which is the
   disagreement lib/field-def.js exists to end. It is translated field by field, refusing each value the way
   any third-party document's value is refused (the bytes are a server's, and lib/persistence.js round-trips
   them through IndexedDB before they reach here).
   ITERATIVE, because a probe's nesting depth is the SERVER's choice: req2proto attaches children from nested
   probes with no depth of its own, so a self-recursive translator would be a stack whose bound an origin
   server sets.

   THE NAME IS THE FIELD RECORD'S OWN AND THE MAP KEY IS NOT A NAME AT ALL — which is the correction, and it
   was not an edge case. lib/req2proto.js's `probeApiEndpoint` DEDUPLICATES its field list by wire tag and
   keys the map it returns `#<number>` for every numbered field (its `key` is `"#" + field.number` where the
   field is numbered and `field.name` only where it is not), so reading that key as the name renamed EVERY
   numbered probe field to the store's own
   dedup token: a field a server described as `userId` reached the Send panel as `#3`, its stated name
   discarded, and `#3` is what lib/encode.js writes as the JSON key (`target[f.name]`, five sites). It also
   broke the merge below — `df.name.toLowerCase() === pf.name.toLowerCase()` cannot match `userid` against
   `#3`, so the probe's number/type/label never overlaid the discovery field and the same field was PUSHED a
   second time, rendering twice: once named and once under a token no document declares. `#3` is not an array
   index, but it is the same fabrication the comment on the old key-vs-name alternative already forbade, and
   it was in the branch that comment believed was the safe one.

   A FIELD THE PROBE NUMBERED AND DID NOT NAME IS REFUSED, and that is a decision rather than a shortfall.
   This record's `name` is one field doing four jobs — the rendered label, the collect key
   (lib/popup-form.js `_collectShallow`), the JSON/GraphQL wire key, and the rename target — which is why
   lib/field-def.js states it among the four with NO absent value. For a numbered-but-unnamed field those
   jobs DISAGREE: the protobuf/JSPB slot is the number and is real, while on the JSON wire there is no key,
   so any name this function supplied would be one the panel invented and sent. The record's one facility for
   a minted name says so in its own words — `isNameMarker` MEANS "addresses NOTHING" and is asserted
   incompatible with a `number` precisely because "a `number` would fill an encoder's slot under it" — so
   routing this case through it would mean weakening that assert to admit the case it names.
   WHAT REFUSING COSTS, STATED AT THE STRENGTH IT WAS CHECKED, because the tempting sentence here is an
   absolute and it is FALSE. An earlier form of this paragraph said refusing loses nothing, on the strength of
   lib/discovery-probe.js's `convertProbeFieldsToSchema` — which does answer this exact question in the
   vocabulary that can state it (it reads `Object.values`, never the key; it skips a field with neither a name
   nor a number; and it files a numbered-but-unnamed one under `field_<n>` with `name: null` and the number
   beside it, a document having a key AND an address where this record has one name for both). But that
   walker runs only in `performProbeAndPatch`, which files its answer under `auto:<service>::<url>`, and the
   lookup below asks `probeResults.get(endpointKey)` — the BARE key, which only `probeEndpoint`'s on-demand
   button writes, and that path builds no virtual document at all. So the one probe answer this function ever
   sees is the one with no second door, and the sentence was a claim about the call graph made where nothing
   could check it. WHAT IS ACTUALLY LOST IS THE SEND-PANEL ROW, and it is a row that could only ever have
   produced a wrong request: it was rendered, collected and SENT under `#<n>`, a key no document declares.
   The OBSERVATION is not lost — the whole probe answer stays in `probeResults`, and lib/popup-discovery.js's
   `_discProbeFieldLabel` already renders exactly this field as `#<n>`, deliberately, as a read-only inventory
   LABEL, which is where a token that addresses nothing belongs and is the distinction this function was
   collapsing.
   `""` is refused with `null` for the reason lib/popup-response.js's `_pbAddressText` and
   lib/discovery-probe.js's schema-name test both give: it is text, and it is an entry no rename can address.

   THE ARRAY SHAPE IS GONE. Its `named === null ? k : named` fallback minted the array INDEX as a wire key —
   which the comment standing over it already called forbidden — and no producer in this tree makes one:
   `probeApiEndpoint` returns `fields: Object.fromEntries(...)`, and lib/store-record.js's `_SR_PROBE_FIELDS`
   states `fields: _srObj`, which excludes an array on the way back out of IndexedDB. A dead arm is not
   revived on the strength of what it would have done; it is refused through `fdDocRecord` like any other
   shape this store does not declare. */
function _probeFieldsToDefs(fieldsObj) {
  /* `null` = THIS PROBE FIELD IS NOT ONE THE PANEL CAN RENDER — a refusal and not an empty record, because
     the caller must not push a row for it. */
  function one(raw) {
    const pf = fdDocRecord(raw) === null ? {} : raw;
    const name = fdDocString(pf.name);
    if (name === null || name === "") return null;
    const type = fdDocString(pf.type);
    const kids = fdDocList(pf.children);
    return {
      def: makeFieldDef({
        name,
        type: type === null ? "string" : type,
        number: fdDocKey(pf.number),
        required: pf.required === true,
        /* A PROBE STATES CARDINALITY OR IT STATES REQUIREDNESS — never a fourth word. `repeated` is the one
           spelling the builder branches on, so anything else the probe wrote is not a cardinality this panel
           can render and the label falls out of `required`, which the line above already decided. */
        label: pf.label === "repeated" ? "repeated" : (pf.required === true ? "required" : "optional"),
        messageType: fdDocString(pf.messageType),
        /* `null` = NOT a message; `[]` = a message the probe found no fields in. The driver below fills it. */
        children: kids === null ? null : [],
      }, "lib/send.js probe field `" + name + "`"),
      kids,
    };
  }
  const out = [];
  const queue = [];
  /* THE VALUES, NEVER THE KEYS. A probe answer's map is keyed by lib/req2proto.js's dedup token, and the
     field's identity is inside the record it points at — the same read `convertProbeFieldsToSchema` makes of
     this very store. `fdDocRecord` is the refusal for a store that is not the shape `_SR_PROBE_FIELDS`
     declares; what it yields is an empty field list, which is the true statement about it. */
  const store = fdDocRecord(fieldsObj);
  for (const raw of store === null ? [] : Object.values(store)) {
    const built = one(raw);
    if (built === null) continue;
    out.push(built.def);
    if (built.kids) queue.push({ into: built.def.children, kids: built.kids });
  }
  while (queue.length > 0) {
    const item = queue.shift();
    for (const raw of item.kids) {
      /* A REFUSED CHILD TAKES ITS OWN CHILDREN WITH IT — a message this panel cannot address is one whose
         nested fields have no path to the wire either, so the subtree is not enqueued. What its parent is
         left holding is `children: []`, which lib/field-def.js reads as "a message with no fields": the
         honest statement here, since `null` would claim the probe never said it was a message, and it did. */
      const built = one(raw);
      if (built === null) continue;
      item.into.push(built.def);
      if (built.kids) queue.push({ into: built.def.children, kids: built.kids });
    }
  }
  return out;
}

function resolveEndpointSchema(endpointKey, service, methodId) {
  // GLOBAL — endpoints/discovery/probes live in the cumulative store keyed by
  // endpointKey/service. Nothing here is per-tab/document (only the network log
  // is); the popup reviews the cumulative cross-site moat.
  /* A MISS ON THIS MAP IS A FACT AND A RECORD IN IT IS COMPLETE — two different statements, and the `?.`
     below used to make them one. `null` here means "this panel is resolving a VIRTUAL endpoint, named by
     service+methodId, that no call site registered"; it never means "the record is there but said nothing". */
  const _epHit = endpointKey ? globalStore.endpoints.get(endpointKey) : undefined;
  const epByKey = _epHit === undefined ? null : _epHit;
  if (epByKey) checkEndpointRecord(epByKey, "lib/send.js resolving a Send-panel schema, key " + JSON.stringify(endpointKey));

  // If no endpoint but we have service+methodId (virtual), create a dummy ep object for context
  if (!epByKey && (!service || !methodId)) return { source: "none", endpoint: null };

  /* THE ENDPOINT'S OWN SERVICE WHEN THERE IS AN ENDPOINT, the caller's argument only when there is none.
     `ep?.service || service` collapsed those into one expression whose two arms answer different questions,
     and its reachable effect was to answer a captured endpoint's discovery lookup with whatever service the
     panel happened to have selected — a lookup against a doc this address was never filed under. */
  const targetService = epByKey ? epByKey.service : service;

  /* THE RECORD THE MATCHED METHOD NAMES, WHICH IS THE ONLY WAY THE SEND PANEL EVER REACHES ONE. The
     parameter above has no producer: the panel's one door is GET_ENDPOINT_SCHEMA and it carries
     (service, methodId), so `epByKey` is null on every request this function actually serves and the
     three consumers of a record below — the templated holes, the probe answer, the projected body — read
     a thing nothing had populated. lib/merge.js stamps `_endpointKey` onto the learned method at the one
     place both the record and the method exist, so the lookup happens BELOW, after `match`, and
     `targetService` is left deciding the doc exactly as it did: resolving the record earlier would let
     `ep.service` pick the document, and merge.js REFINES an endpoint's service after the doc was filed
     under the unrefined name, so a refined one would miss the doc and the row would vanish from the panel.
     AN ABSENT `_endpointKey` IS A FACT AND NOT A GAP. lib/persistence.js round-trips these documents
     through IndexedDB, so a method written before the stamp existed carries none and resolves to no
     record — which is exactly the state this function has always been in, stated rather than crashed on. */
  let ep = epByKey;
  let epKeyUsed = endpointKey ? endpointKey : null;

  let source = "none";
  let discoveryMethod = null;
  let parameters = null;
  let bodyFields = null;
  let bodySchemaName = null;
  let contentTypes = [];

  // 1. Discovery doc — global, per-service (the discovery store is not per-tab).
  const discoveryEntry = globalStore.discoveryDocs.get(targetService);
  if (discoveryEntry?.doc) {   // the method surface is the doc, never the published fetch's status (lib/serialize.js)
    const doc = discoveryEntry.doc;
    let match = null;

    if (methodId) {
      // Direct lookup by ID (virtual endpoint)
      match = findMethodById(doc, methodId);
    } else if (epByKey) {
      // Path matching (captured endpoint)
      /* THE VERB IS ON THE RECORD. `|| "POST"` matched this address against the discovery doc's POST method
         whenever the endpoint's own verb went missing — a GET endpoint resolved to a POST method's schema,
         which the Send panel then renders as this endpoint's parameters. lib/merge.js asserts the call site
         carries a method before it registers one, so there is no absence here to answer for. */
      match = findDiscoveryMethod(doc, epByKey.path, epByKey.method);
    }

    if (match) {
      source = "discovery";
      /* THE RECORD, BY THE NAME THIS METHOD CARRIES. A miss is a fact — a document persisted before the
         stamp, or a record the moat has since dropped — and leaves `ep` exactly as it was. A HIT whose verb
         disagrees is not a fact about the store, it is this stamp naming somebody else's record, and every
         field projected below would then describe a request at a different address. */
      const _mKey = match.method._endpointKey;
      if (ep === null && typeof _mKey === "string" && _mKey !== "") {
        const _mHit = globalStore.endpoints.get(_mKey);
        if (_mHit !== undefined) {
          checkEndpointRecord(_mHit, "lib/send.js resolving the record a learned method names, key " +
                                     JSON.stringify(_mKey));
          DCHECK(_mHit.method === match.method.httpMethod,
                 "the learned method " + JSON.stringify(match.method.id) + " names endpoint record " +
                 JSON.stringify(_mKey) + ", whose verb is " + JSON.stringify(_mHit.method) + " and not " +
                 JSON.stringify(match.method.httpMethod) + " — lib/merge.js stamps that name inside the loop " +
                 "that mints both, so a disagreement is one record's name on another's method and the Send " +
                 "panel would show this address beside a different request's body and holes");
          ep = _mHit;
          epKeyUsed = _mKey;
        }
      }
      discoveryMethod = {
        id: match.method.id,
        httpMethod: match.method.httpMethod,
        path: match.method.path || match.method.flatPath,
        description: match.method.description,
        scopes: match.method.scopes || [],
        resourceName: match.resourceName,
        contentTypes: match.method.contentTypes || [],
        // AST-learned required headers (the header SET the bundle attached at
        // the host edge, per-header literal/opaque) — transport metadata the
        // Send panel surfaces. Whitelisted out before, so the popup never saw
        // it for AST endpoints (which load via service+methodId, ep=null).
        requiredHeaders: match.method.requiredHeaders || null,
      };

      // Resolve parameters
      if (match.method.parameters) {
        parameters = {};
        /* A DISCOVERY METHOD'S PARAMETER IS A THIRD-PARTY DOCUMENT, AND `||` NORMALISED ONLY ITS SILENCE.
           `pDef.enum || null` answers "the document declared no membership" and "the document's `enum` is
           the string `admin`" with the same bytes, and the second then travels to a panel that renders a
           select out of it — the same two-claims-one-appearance defect the projection below was written to
           end, one layer up from where it was fixed. Every value is now REFUSED rather than defaulted
           (lib/field-def.js): a value in a form this record does not carry states nothing, and `null` is the
           true statement about it. It must be a refusal and not an assert, because these bytes are a
           document the target's server served or a spec file the researcher was handed, and the trusted zone
           does not abort on somebody else's input. */
        for (const [pName, pDef] of Object.entries(match.method.parameters)) {
          const pd = fdDocRecord(pDef) === null ? {} : pDef;
          const pAlias = fdDocString(pd.name);
          const pType = fdDocString(pd.type);
          const pLoc = fdDocString(pd.location);
          const pDesc = fdDocString(pd.description);
          parameters[pName] = {
            name: pAlias === null ? pName : pAlias,
            customName: pd.customName === true,
            /* THE FIELD NUMBER A FORM-BODY PARAMETER CARRIES. lib/openapi-import.js writes it onto the
               parameter verbatim from an imported spec's `x-field-numbers`, and lib/openapi-export.js reads
               it back on the way out — so it is a fact this record has always held and this projection had
               never carried, which meant an imported number survived a round-trip through the store and was
               dropped on the one path that RENDERS it. `null` = this parameter is not numbered. */
            number: fdDocKey(pd.number),
            type: pType === null ? "string" : pType,
            /* "query" IS THE SPEC'S OWN ANSWER, NOT A FILLER: an OpenAPI parameter must state `in`, and a
               Google discovery parameter without a `location` is a query parameter. It is what a document
               that names none has said, which is why it is not `null`. */
            location: pLoc === null ? "query" : pLoc,
            required: pd.required === true,
            description: pDesc === null ? "" : pDesc,
            format: fdDocString(pd.format),
            enum: fdDocList(pd.enum),
            // Stats-derived metadata
            _requiredConfidence: typeof pd._requiredConfidence === "number" ? pd._requiredConfidence : null,
            _detectedEnum: pd._detectedEnum === true,
            _defaultValue: pd._defaultValue === undefined ? null : pd._defaultValue,
            _defaultConfidence: typeof pd._defaultConfidence === "number" ? pd._defaultConfidence : null,
            _range: fdDocRecord(pd._range),
            // Unified example value (pickExampleValue result) — popup
            // uses this to prefill the Send form so reviewers can
            // send a plausible request without first replaying a
            // captured one. The source tag lets the UI label the
            // prefill (observed / ast / synthesized / type-default).
            _exampleValue: pd._exampleValue === undefined ? null : pd._exampleValue,
            /* READ AGAINST THE VALUE IT ATTRIBUTES — `fdDocString` asks only what TYPE this half is, and a
               discovery document is free to name this half and no `_exampleValue` at all, which would put a
               prefill badge over a value that came off the wire. */
            _exampleValueSource: fdDocExampleSource(pd._exampleValue, pd._exampleValueSource),
            // AST-discovered valid values
            _astValidValues: fdDocList(pd._astValidValues),
            /* …AND THE POOL BESIDE IT, WHICH IS THE SAME OBSERVATION AT THE ONE GRADE THAT MUST NOT BE
               OFFERED. lib/learn.js keeps a value every sighting of which stood on a FORCED arm out of the
               list above, because the panel prefills from that list and a forced example is a request no
               client makes — but the value is still a fact this run computed and the panel says so on its
               own row. A projection that carried only the offerable pool would make a forced-only parameter
               and one nothing ever learned render with identical bytes, which is §@H's wrong report rather
               than a thin one. */
            _astForcedValues: fdDocList(pd._astForcedValues),
            /* …and the drive mark over the offerable pool: the values only this tool's drives computed. */
            _astDrivenValues: fdDocList(pd._astDrivenValues),
            /* AND THE OTHER HALF OF THE SHAPE. A param's two facts are PROVENANCE-and-example (above) and
               DOMAIN — what the forced execution's own equality gates proved this value is NOT. A projection
               that carried only the first would put the popup back where the engine was before it emitted
               one: a range-gated parameter and an unconstrained one rendering with identical bytes. Written
               like `_exampleValue` rather than with a `||`, because an empty array must not arrive here as
               "nothing was observed" — lib/learn.js writes `[]` to mean a claim that was DISPROVED. */
            _excludedValues: fdDocList(pd._excludedValues),
            /* …AND THE SAME FACT OVER AN ORDERED DOMAIN. `_range` above is what live traffic's observed
               values SPANNED — a statistic; this is what the bundle's own ordering gates REQUIRE, on every
               path the engine observed reaching the request. The two are different claims and are carried
               apart so the popup can say which is which; collapsing them would state a constraint out of a
               sample. Written like `_exampleValue` rather than with a `||`, because lib/learn.js writes
               `null` to mean a claim that was DISPROVED and `undefined` to mean nothing was ever observed. */
            _bounds: fdDocRecord(pd._bounds),
            /* …AND THE THIRD OF THE THREE — the METHOD-CALL gates the bundle's own code ran on this value and
               the arm each answered. An equality gives `_excludedValues`, an ordering `_bounds`, and a call
               neither, so a projection carrying the first two and not this one leaves a parameter gated only
               by `startsWith("/api")` rendering exactly like one nothing had ever tested. Written like
               `_excludedValues` rather than with a `||` for the reason stated there: `[]` is a claim another
               path DISPROVED and must not arrive as "nothing was observed". */
            _predicates: fdDocList(pd._predicates),
            /* …AND THE FOURTH — the LOOSE EQUALITIES (`==`) the bundle's own code HELD of this value.
               ECMAScript §7.2.14 IsStrictlyEqual step 1 makes a `===` that held a DETERMINATION, so it
               arrives as a value; §7.2.13 IsLooselyEqual coerces and its holding arm determines none, so the
               engine records the predicate — and a projection carrying the first three and not this one
               leaves a parameter gated only by `x == 0` rendering exactly like one nothing had ever tested.
               Written like `_excludedValues` for the reason stated there. */
            _looselyEquals: fdDocList(pd._looselyEquals),
            /* …AND THE KEY THAT SAYS WHICH OF TWO SENTENCES THOSE FOUR SILENCES ARE, WHICH IS NOT A FIFTH
               ONE OF THEM. solver/endpoint.c reads all four domains through the param's HOLE KEY, so a
               parameter whose value the code COMPUTED had all four skipped at the mint and their absence
               stops meaning "no gate of that kind survived every observed path" — it means "a literal, with
               nothing to look a domain up on". The two take OPPOSITE work (the first wants more gates
               observed, the second wants nothing however good the engine gets) and without this key they
               reach the panel with identical bytes, which is §@H's wrong report rather than a thin one.
               READ THROUGH A REFUSAL AND NOT AN ASSERT, for the reason every line above it is: this record
               may be a Google discovery document the target's server served or an OpenAPI file the
               researcher was handed, and a document naming a third spelling is refused into the record's
               stated absence rather than crashing the trusted zone. */
            _astValueClass: fdDocValueClass(pd._astValueClass),
            /* NO `_sourceMapName` AND NO `_astValueSource`. The first promised a declared name recovered
               from the page's source map (minified `e` shown as `owner`); nothing in engine/host has ever
               emitted one and lib/learn.js's copy of it read a field the engine's param record does not
               have, so the rename has never been applied to any parameter. The second was written in
               exactly one place — this file, onto its own path-param entries — projected onward by
               popup-form.js, and rendered by nothing: three hops and no reader. */
          };
        }
      }

      // Resolve request body schema
      if (match.method.request?.$ref) {
        bodySchemaName = match.method.request.$ref;
        bodyFields = resolveDiscoverySchema(doc, bodySchemaName);
      }
    }
  }

  // 2. Try probe results (only if we have a real endpoint key)
  const probeResult = epKeyUsed
    ? globalStore.probeResults.get(epKeyUsed)
    : null;
  if (probeResult?.fields) {
    /* A PROBE'S FIELD LIST IS NOT OURS EITHER. lib/req2proto.js learns it by reading a `google.rpc.Status`
       the SERVER composed, and lib/persistence.js round-trips it through IndexedDB, so it reaches this line
       as bytes neither of which we wrote — refused through lib/field-def.js rather than asserted, exactly as
       a discovery document is. It becomes a WHOLE FieldDef here, which is the change: it used to state nine
       names and the form builder read the other twenty through `||`, so a probe-learned field and a
       discovery-declared unconstrained one rendered with identical bytes while only the second had ever been
       asked about a domain. */
    const probeFields = _probeFieldsToDefs(probeResult.fields);

    if (!bodyFields || bodyFields.length === 0) {
      // No discovery body fields — use probe fields directly
      source = source === "discovery" ? "merged" : "probe";
      bodyFields = probeFields;
    } else {
      // Merge: overlay probe field numbers onto discovery fields
      source = "merged";
      for (const pf of probeFields) {
        const match = bodyFields.find(
          (df) => df.name.toLowerCase() === pf.name.toLowerCase(),
        );
        if (match) {
          if (pf.number) match.number = pf.number;
          if (pf.type !== "unknown" && match.type === "string")
            match.type = pf.type;
          if (pf.label === "repeated") match.label = "repeated";
          if (pf.children && !match.children) match.children = pf.children;
        } else {
          bodyFields.push(pf);
        }
      }
    }
  }

  // 3. Content type suggestions — prefer method-level observed CTs
  if (discoveryMethod?.contentTypes?.length) {
    for (const ct of discoveryMethod.contentTypes) {
      if (!contentTypes.includes(ct)) contentTypes.push(ct);
    }
  }
  /* `pd.contentType &&` STOOD HERE AND COULD NEVER BE FALSE, WHICH IS WHY IT IS AN ASSERT NOW AND NOT A GUARD.
     A truthiness test in front of a producer's field turns "the producer did not produce this" into a silent
     skip, and a skipped probe detail is indistinguishable from one that was never there — the content-type
     list simply comes back SHORT, with nothing anywhere saying so. That is the quiet half of
     §A-FIELD-A-CONSUMER-DEFAULTS, and this record reaches here out of the RESTORED store
     (`globalStore.probeResults.get(endpointKey)` above), which is the one place a record can arrive short of a
     name with no producer in this session having gone wrong.
     THE PRODUCER SETTLES WHICH FIX THIS IS, AND IT WAS GREPPED RATHER THAN INFERRED FROM THIS READER. Both
     `results.push` sites in lib/req2proto.js set `contentType` from a non-empty string LITERAL — the three
     `ct:` configs, and a ternary between two more for the nested probe — and no `sendProbe` return carries a
     `contentType` key, so the `...result` spread that follows cannot overwrite it. `fieldCount` is
     `r.fields?.length || 0`, always written and always a number. So the producer CANNOT legitimately omit
     either name: there is no absence here to read positively, and the remedy is the assert rather than a
     positive reading of a gap.
     AND NO STORED RECORD CAN LACK THEM EITHER, which is what makes asserting safe rather than a regression
     delivered as a fix. The mint is byte-identical at every revision where the file exists — the first commit
     that introduced it states the same five names as today, and the one revision in between is the commit that
     emptied the tree and its restore. So this cannot fire on a record any build ever wrote, and if it ever
     does, the producer stopped writing a name this list is composed from.
     `pd.fieldCount > 0` IS NOT A DEFAULT AND STAYS. It is the semantic filter this loop is for: only a content
     type that actually yielded fields is worth suggesting, and 0 is a real answer meaning the probe got
     nothing back. Deleting it would widen the suggestion list, which is a different change and not this one. */
  if (probeResult?.probeDetails) {
    for (const pd of probeResult.probeDetails) {
      DCHECK(typeof pd.contentType === "string" && pd.contentType !== "" &&
             typeof pd.fieldCount === "number",
        "a stored probe detail reached the send panel without its content type or field count — " +
        "lib/req2proto.js writes both on every entry of `probeDetails` and has since the mint was " +
        "introduced, so absence is that producer or the store's restore broken, and this panel would " +
        "otherwise skip the entry in silence and suggest a SHORT content-type list that reads exactly like " +
        "a probe which found nothing (contentType=" + JSON.stringify(pd.contentType) +
        " fieldCount=" + JSON.stringify(pd.fieldCount) + ")");
      if (pd.fieldCount > 0 && !contentTypes.includes(pd.contentType)) {
        contentTypes.push(pd.contentType);
      }
    }
  }
  if (!contentTypes.length) {
    contentTypes = [
      "application/json",
      "application/json+protobuf",
      "application/x-protobuf",
    ];
  }

  // 4. Collect chain data from the raw method object
  let chains = null;
  if (discoveryEntry?.doc && methodId) {
    const rawMatch = findMethodById(discoveryEntry.doc, methodId);
    if (rawMatch?.method?._chains) {
      chains = rawMatch.method._chains;
    }
  }

  /* AST-LEARNED PATH-HOLE EXAMPLES OFF THE FLAT ENDPOINT RECORD — BOTH POOLS. The rich per-document method
     schema above is the better source when it declares the hole; this is what the panel has when it does not,
     which is a real and reachable state rather than a lifetime story: the discovery match is by PATH, so a
     partial match resolves a method that declares other segments, and an address the moat unioned from
     another document carries holes whose method lives in a bucket this service's doc does not hold. On every
     one of those the flat record is the WHOLE statement about what the run learned for the segment.
     WHICH IS WHY THE FORCED POOL HAD TO REACH HERE, and the sentence that stood here — "they survive doc
     eviction" — is a scar of a reclaim that is deleted (offscreen-brain.js records it), not the reason. The
     reason is §@H's: a shape states provenance AND domain, and this block used to state `_astForcedValues:
     null` for every hole it declared, which lib/popup-form.js renders as "every value learned for this field
     was computed on a path that stood on no forced arm". For a hole the run could only fill by forcing a gate
     that is the INVERSE of what was observed — a wrong report, not a thin one — and the value the run did
     compute reached no reviewer at all. */
  const _epHoles = ep ? endpointHolePairs(ep, "lib/send.js resolving the Send-panel schema for " +
                                              JSON.stringify(epKeyUsed)) : null;
  /* AND WHAT THE RUN'S OWN GATES PROVED ABOUT THOSE HOLES, off the same record and read here rather than
     re-derived, because the literal below is where its absence was being STATED. §@H: a shape states
     PROVENANCE and DOMAIN and dropping either is a wrong report; this block had the first and wrote `null`
     for all four of the second, which lib/popup-form.js renders as no badge and no placeholder — a segment
     the run had narrowed to `> 5` reaching a reviewer with the same bytes as one nothing ever tested. */
  const _epDomains = ep ? endpointHoleDomains(ep, "lib/send.js resolving the Send-panel schema for " +
                                                  JSON.stringify(epKeyUsed)) : null;
  /* THE WALK IS THE UNION OF THE TWO NAME SETS AND NOT THE POOLS' KEYS — the named residual
     lib/endpoint-record.js states at `pathParamDomains`, retired here, and keying it on the pools alone was a
     WRONG report rather than a thin one. lib/merge.js builds the two lists by two different SKIP rules and
     says so at its own mint: a hole with values and no domain is in the pools and not in the domain list, and
     a hole with a domain and no value is in the domain list and in NEITHER pool, because every pool entry is
     required to carry a value. So the second population arrived on the record complete and declared no
     parameter at all: the panel offered no box for a segment this run had narrowed, and the address kept a
     literal `{name}` nothing could fill. §@H's line is whether a VALUE was determined, never whether a
     CONSTRAINT was — `x > 5` on the arm this flow took is a fact the run OBSERVED exactly as a pin is, so the
     domain owes a field, and inventing `6` to put in it is the fabrication that rule forbids by name.
     THE PAIR IS EMPTY FOR SUCH A NAME AND THAT IS THE RECORD'S OWN STATEMENT RATHER THAN A `||`. The pools do
     not name this hole, which is the positive claim that no path filled it at either grade — the same claim
     `endpointHolePairs` makes by minting no entry. `_NO_HOLE_POOLS` is the one spelling of it
     (lib/endpoint-record.js, beside that walk), shared with lib/merge.js's moat fold, which asks the identical
     question one seam over and would otherwise be a second answer to it.
     AND A HOLE WITH VALUES AND NO DOMAIN IS UNCHANGED BY THIS, which is the controlled pair: it was already in
     the set the pools' keys produce, it still reads the all-null `_hd` below, and it goes through the same
     fold with the same operands. Neither direction has a branch of its own, so neither can drift from the
     other — the only thing this diff changes is WHICH NAMES reach the loop. */
  const _holeNames = new Set();
  if (_epHoles !== null) for (const _n of _epHoles.keys()) _holeNames.add(_n);
  if (_epDomains !== null) for (const _n of _epDomains.keys()) _holeNames.add(_n);
  if (_holeNames.size) {
    parameters = parameters || {};
    for (const _hn of _holeNames) {
      const _hp = (_epHoles !== null && _epHoles.has(_hn)) ? _epHoles.get(_hn) : _NO_HOLE_POOLS;
      /* A NAME THE RESOLVED SCHEMA DOES NOT DECLARE IS NOT A HOLE — it is the schema stating it has no such
         parameter, and a templated path segment the engine learned a value for is one the panel must show
         anyway. So the miss is answered by DECLARING the parameter out of what this hole's entry already
         states, never by defaulting one into existence behind a `||`: `location` is "path" because that is
         the list the entry came from, `required` is true because a templated segment with no value does not
         produce a URL at all, and `type` is "string" because a path segment IS the URL's own bytes.
         Each is a fact about a path parameter rather than a guess about this one — §RUN, DON'T MATCH: what
         is surfaced is what merge.js observed plus what the URL grammar determines. */
      /* AND IT DECLARES THE WHOLE RECORD, NOT THE FIVE FIELDS IT HAS AN INTERESTING ANSWER FOR. This literal
         and the discovery-doc literal above are the map's TWO producers, and they were writing two different
         shapes: everything the doc-derived branch says about a parameter's domain — `enum`, `format`,
         `_range`, `_bounds`, `_excludedValues`, `_predicates`, `_looselyEquals`, the three confidences, the example and its
         source — was simply absent here. The consumers papered over the disagreement rather than reporting it
         (`param.enum || null`, `param._range || null`, `param._defaultValue ?? null` in lib/popup-form.js),
         which is the §Architecture defect exactly: a default is what stops a producer's silence being a
         crash, and here the silence was STRUCTURAL — one of the two producers had never written the field
         at all, so the panel could never tell "this parameter is unconstrained" from "this branch does not
         speak about constraints".
         NULL IS A POSITIVE STATEMENT AND IT IS THE TRUE ONE. A path parameter's domain is not unknown, it is
         EMPTY: nothing observed a discovery declaration for this segment, so there is no enum, no format, no
         range, no ordering bound and no disproved-value list to report — the same `null` lib/merge.js writes
         for a claim nothing has filled. BOTH POOLS ARE PRE-DECLARED AND NEITHER IS SPECIAL: the fold below
         reads each one back before it writes it, so a literal that named only one of them would be a producer
         that speaks half this record's vocabulary — which is the asymmetry that stood here, and it was the
         half the endpoint had no way to fill. */
      const declared = Object.prototype.hasOwnProperty.call(parameters, _hn);
      /* A HOLE THE RECORD STATES NO DOMAIN FOR IS `null` AT ALL FOUR, which is this parameter's own spelling
         of "nothing proved" and not a hole a `||` filled: lib/endpoint-record.js's walk returns `null` per
         claim for a hole it names, and a `Map` miss is that record stating it narrowed this segment not at
         all. Both are the same statement to a FieldDef, which is why one literal serves them. */
      const _hd = (_epDomains !== null && _epDomains.has(_hn)) ? _epDomains.get(_hn)
                : { excludes: null, bounds: null, predicates: null, looselyEquals: null };
      /* NAMED RESIDUAL — THE FOUR CLAIMS LAND ONLY ON A PARAMETER THIS BRANCH DECLARES.
           WHAT IS NOT COVERED: a hole whose name the resolved schema ALREADY declares. `_hd` is read for it
             and then dropped, because `cur` is the doc-derived parameter and only the literal below writes
             the four onto one. That parameter carries the domain of THIS DOCUMENT'S matched method
             parameter; this record's entry is the moat's fold of the same four across every document that
             reached the address (lib/merge.js intersects the three lists and WIDENS the interval to the
             hull). They are two claims about one segment, and the panel renders one of them with nothing
             saying the other was asked.
           WHAT THE NEXT DIFF BUILDS: that fold, at this attach, ROUTED to the rules that already spell it
             rather than spelled a third time — `intersectExcludedValues` / `widenBoundsInto` /
             `intersectPredicates` / `intersectLooselyEquals` (lib/learn.js), over `cur`'s four and `_hd`'s,
             which is exactly what `_foldEndpointRecordInto` calls one seam over for the two records' own
             copies. The declaration would then meet the record's by the law that governs both, instead of
             winning by having been written first.
           HOW ITS ABSENCE WOULD SHOW: a declared path parameter whose badge states a constraint that the
             same address's `pathParamDomains` entry for that name does not carry — readable side by side in
             one @RESULT, on any record the moat folded from more than one document. */
      const cur = declared ? parameters[_hn]
                           : { name: _hn, customName: false, number: null, type: "string", location: "path",
                               required: true, description: "AST-learned path segment",
                               format: null, enum: null,
                               _requiredConfidence: null, _detectedEnum: false,
                               _defaultValue: null, _defaultConfidence: null, _range: null,
                               _exampleValue: null, _exampleValueSource: null,
                               /* `enum: null` IS RIGHT HERE AND ITS REASON IS NOT WHAT THIS PARAGRAPH SAID.
                                  It read: a templated segment has no enum, no format and no observed range,
                                  "those come from a DISCOVERY DECLARATION or from traffic statistics and
                                  there is none for a hole". That names TWO of the THREE producers of an
                                  `enum` in this extension and misses the one that actually fires for a
                                  templated path hole — lib/learn.js's `_mergeAstValues`, which promotes a
                                  hole's AST-learned values to a membership at two distinct values, and which
                                  lib/learn.js's own templated-path reconcile drives by dissolving a concrete
                                  sibling address into the template. A hole CAN carry an inferred enum, and
                                  the sentence said it cannot.
                                  THE CONCLUSION SURVIVES THE CORRECTION AND IT IS DECIDED ONE LINE UP, NOT
                                  HERE. All three producers write onto a METHOD RECORD parameter, and this
                                  literal is the arm where `declared` is FALSE — the matched method has no
                                  such parameter — so by construction no producer has ever run for this name
                                  and `null` is the true statement. The DECLARED arm is the one that carries
                                  a promoted membership, and it carries it through `parameters[pName].enum`
                                  above, which is why nothing is missing here. Recorded rather than quietly
                                  reworded because the retired reason is re-derivable from this file alone: a
                                  reader who re-checks "does a hole have an enum source" against the two
                                  sources named here will conclude it has none and write this sentence again.
                                  NAMED RESIDUAL — THE FOLD BELOW WIDENS THE POOL AND LEAVES A STANDING
                                  MEMBERSHIP STALE.
                                    WHAT IS NOT COVERED: the DECLARED arm. `cur` is then the method's own
                                      parameter, `foldValuePools` merges this endpoint record's pools into
                                      `cur._astValidValues`, and `cur.enum` is not restated — so a value that
                                      reached the flat record and not the method is in the pool and outside
                                      the membership. lib/popup-form.js's `createSingleInput` tests `enum`
                                      FIRST and returns a `<select>`, so the `<datalist>` arm is unreachable
                                      for a field carrying both and a `<select>` takes no free text: the
                                      value is neither offered nor typeable. lib/learn.js now restates an
                                      INFERRED membership as its own pool grows, which closes this inside
                                      that file; this fold is the third site and is not covered by it.
                                    WHAT THE NEXT DIFF BUILDS: the same two predicates this file does not yet
                                      reach — `enumClaimIsDeclared` and `_restatementWidens` (lib/learn.js) —
                                      applied to `cur` after the fold, so an inferred membership is restated
                                      from `_f.valid` and a declared one is left alone. It is `_f.valid` and
                                      never `_f.forced`: the offerable pool is the only operand a membership
                                      may take, which is the split this fold exists to keep.
                                    HOW ITS ABSENCE WOULD SHOW: a path parameter rendered as a select whose
                                      option list is shorter than the value list the same record carries in
                                      `_astValidValues`, on an address whose endpoint record was unioned from
                                      more documents than the method was.
                                  THE FOUR DOMAIN FACTS ARE THE RECORD'S, NOT A STATED EMPTY. These four do not: they are the
                                  ENGINE'S own forced execution, merged by lib/learn.js onto the very method
                                  parameter lib/merge.js copies the value pools from, and they arrive here on
                                  the flat record. `null` for them was one sentence covering two sources and
                                  true of only one — for the other it asserted that no gate of that kind held
                                  on every observed path to a segment the run may well have narrowed, which
                                  §@H calls a WRONG report rather than a thin one. A hole with no entry, and a
                                  hole whose entry carries no claim of a kind, both read `null` here: the
                                  record spells a disproved claim by omitting the key, so `null` IS this
                                  parameter's "nothing proved" (lib/field-def.js) at both. */
                               _excludedValues: _hd.excludes, _bounds: _hd.bounds,
                               _predicates: _hd.predicates, _looselyEquals: _hd.looselyEquals,
                               _astForcedValues: null,
                               _astValidValues: null, _astDrivenValues: null };
      DCHECK(cur && typeof cur === "object",
             "the resolved schema declares `" + _hn + "` as something that is not a parameter record — " +
             "resolveEndpointSchema builds this map out of parameter declarations, so a non-object here is " +
             "that build broken and a learned path value is about to be attached to it");
      /* `null` IS "NOTHING HAS BEEN LEARNED FOR THIS PARAMETER YET", AND IT IS THE ONLY SPELLING OF IT.
         This assert used to accept `undefined`-or-array, which is the vocabulary lib/merge.js's
         `_mergeParamInto` speaks (it writes the field only where the union is non-empty) — but merge.js is
         not this map's producer. BOTH producers are literals in this function, and the doc-derived one
         normalises through `pDef._astValidValues || null`, so the value arriving here is `null` and the
         assert REJECTED IT. That is not a theoretical arm: `ep.pathParams` lives on the ENDPOINT record and
         the comment above says why — it survives the eviction of the per-document method schema — so a
         parameter the endpoint learned values for, read back against a doc whose own copy has none, is
         exactly the case this whole block exists to serve, and it aborted the Send panel in dev on it.
         Two spellings of one fact with an assert that knew one of them is the defaulted-field defect turned
         inside out: not a hole silently filled, but a legitimate value refused. The record now says `null`
         from both producers, so `undefined` here means a producer stopped writing the field. */
      const priorVals = cur._astValidValues, priorForced = cur._astForcedValues;
      const priorDriven = cur._astDrivenValues;
      DCHECK((priorVals === null || Array.isArray(priorVals)) &&
             (priorForced === null || Array.isArray(priorForced)) &&
             (priorDriven === null || Array.isArray(priorDriven)),
             "a parameter's `_astValidValues`/`_astForcedValues` is neither null nor an array — " +
             "resolveEndpointSchema's two parameter literals both write both (null when nothing was observed, " +
             "the pool otherwise), so a third spelling is one of them broken and the panel would render a " +
             "learned-value list nobody observed, or a forced one as offerable");
      /* THE ATTACH IS A FOLD AND NOT TWO UNIONS, which is the whole reason `foldValuePools` exists. This is
         the FOURTH place two sightings of one value meet — the schema's pools, from the method record, and
         the endpoint's, from a record that may have been unioned across documents and sessions — and the two
         can legitimately disagree about a value's grade. Unioning each pool into its namesake would then put
         the same string in both, which lib/field-def.js's own disjointness DCHECK rejects at makeFieldDef and
         which the panel would render as one value offered as the app's and labelled as no client's. */
      /* …and the drive marks: the schema's over its pool, the flat record's (`pathParamsDriven`) over its. */
      const _f = foldValuePools(priorVals === null ? [] : priorVals,
                                priorForced === null ? [] : priorForced, _hp.valid, _hp.forced,
                                priorDriven === null ? [] : priorDriven, _hp.driven);
      /* AN EMPTY POOL IS WRITTEN `null`, WHICH IS THIS MAP'S ONE SPELLING OF "nothing was learned at this
         grade" — the same one both literals above declare. A forced-only hole therefore leaves
         `_astValidValues: null` rather than `[]`, and there is nothing for the prefill below to take, which
         is exactly right: §@H permits a domain-annotated shape with no example and forbids inventing one. */
      cur._astValidValues = _f.valid.length ? _f.valid : null;
      cur._astForcedValues = _f.forced.length ? _f.forced : null;
      cur._astDrivenValues = _f.driven.length ? _f.driven : null;
      /* THE PREFILL IS THE PAIR, AND IT IS WRITTEN ONLY WHERE BOTH HALVES ARE TRUE. `fdCanStateExample`
         (lib/field-def.js) is the same question `pickExampleValue` asks of its own two list tiers: a pool
         member of `null` written here would land as `_exampleValue: null` — the record's own spelling of
         "nothing was computed" — beside `_exampleValueSource: "ast"`, and makeFieldDef refuses that pair by
         name because the Send panel badges off the SOURCE and would attribute the captured request's value
         to this run. An unstatable head leaves the field with no example, which is what it has. */
      if ((cur._exampleValue === undefined || cur._exampleValue === null) &&
          _f.valid.length && fdCanStateExample(_f.valid[0])) {
        cur._exampleValue = _f.valid[0];
        cur._exampleValueSource = "ast";
      }
      parameters[_hn] = cur;
    }
    if (source === "none") source = "ast";
  }

  return {
    source,
    method: discoveryMethod,
    parameters,
    requestBody: bodyFields?.length
      ? { schemaName: bodySchemaName, fields: bodyFields }
      : null,
    contentTypes,
    chains,
    /* WHAT AN ENDPOINT RECORD ACTUALLY HAS. lib/merge.js holds the only `endpoints.set` in the extension and
       writes exactly the names lib/endpoint-record.js declares — read them THERE, because that file's header
       records what transcribing them costs and `makeEndpointRecord` refuses anything else.
       Five more names were projected here — apiKey, apiKeySource, origin, referer, contentType — and no
       producer writes any of them onto an endpoint: lib/response-decode.js does read those four headers off
       a live request, but it puts them on the REQUEST LOG entry, which is a different record. So the panel
       has been handed five undefineds per endpoint, and `ep?.apiKey || null` above turned the first into the
       "no key for this endpoint" answer that sends the resolver down its service-wide key search — the path
       that actually finds keys, which is why nothing looked wrong.
       `requiredHeaders` is genuinely optional (merge.js writes null when endpoint.c observed no header), so
       it passes through as itself rather than through a `||` that would erase the difference. */
    endpoint: ep
      ? {
          url: ep.url,
          method: ep.method,
          host: ep.host,
          path: ep.path,
          service: ep.service,
          requiredHeaders: ep.requiredHeaders,
          /* THE BODY THIS ENDPOINT WAS OBSERVED SENDING, WHICH THIS LIST IS THE HOP IT WOULD OTHERWISE HAVE
             DIED AT. The list is an explicit allowlist, so a name the record carries and this omits reaches
             no surface at all and reads there as the record's declared absence — the same silent drop the
             five names above this comment record, arriving from the opposite direction: those were names
             NOTHING wrote being read, and this is a name something writes not being carried.
             BOTH, NEVER ONE. They are the two arms of one question (bytes the request sends, against the
             display spelling of a body composed from unknown input) and the record asserts they are
             exclusive; projecting only the first would answer "no body" for every unknown-composed payload,
             which is the case a reviewer most needs named because a sniffer can never produce it. Each
             passes through as itself rather than through a `||`, for `requiredHeaders`' reason one line up:
             null is a stated absence and collapsing it to a falsy default erases which of the two it was. */
          bodySent: ep.bodySent,
          bodyShape: ep.bodyShape,
          /* AND THE THIRD, FOR THE SAME REASON AND WITH A SHARPER ONE OF ITS OWN. The list is an explicit
             allowlist, so a name the record carries and this omits reaches no surface at all and reads there
             as the record's declared absence. For `bodyExample` that silent drop would be the worst of the
             three: the endpoint's `body[off:end]` params DO reach the panel (they are the method's request
             schema), so the reviewer would get the addresses with nothing at them — which is exactly the
             state solver/endpoint.c's residual was written against, reproduced one hop later by an omission. */
          bodyExample: ep.bodyExample,
        }
      : null,
  };
}

// ─── Send Request: Body Encoding ─────────────────────────────────────────────

/**
 * Encode form fields as a JSON object (field names as keys).
 */
// (Request encoding -- encodeGraphQLBody/encodeFormToJson/Jspb/Protobuf + pb-field encoders -- extracted
// to lib/encode.js, loaded first.)

function coerceValue(value, type) {
  if (value == null) return null;
  if (type === "bool" || type === "boolean") return value === true || value === "true";
  if (type === "enum") {
    var n = Number(value);
    return isNaN(n) ? String(value) : n;
  }
  if (
    type === "number" ||
    [
      "int32",
      "int64",
      "uint32",
      "uint64",
      "double",
      "float",
      "sint32",
      "sint64",
      "fixed32",
      "fixed64",
      "sfixed32",
      "sfixed64",
    ].includes(type)
  ) {
    // Already a number? Preserve exactly — `Number(42)` → 42, but
    // `Number("42")` also → 42 and crucially `String(42)` would emit `"42"`
    // which breaks JSON byte-equivalence.
    return typeof value === "number" ? value : Number(value);
  }
  // Numeric-typed JSON values without an explicit scalar-typed field still
  // need to stay numbers. Same for booleans and null-ish passthroughs.
  if (typeof value === "number" || typeof value === "boolean") return value;
  return String(value);
}

// ─── Send Request: Execute ───────────────────────────────────────────────────

/* THE SEND PANEL'S REQUEST, STATED ONCE FOR THE TWO COMMANDS THAT CARRY IT.
 *
 * `SEND_REQUEST` (executeSendRequest, below) and `BUILD_REQUEST` (buildExportRequest, in offscreen-brain.js)
 * are the SAME record read by two consumers — one fires it, one renders it as curl/fetch/python — and both
 * were reading it through `||`, in two copies that had to be kept in step by hand. Every one of those
 * defaults substituted an OPERATOR CHOICE: `msg.httpMethod || "POST"` picks a verb, `{ ...(msg.headers || {})
 * }` drops the header rows they typed, `msg.contentType?.startsWith(…)` skips the encoding they selected. The
 * popup holds each of those in a variable that is initialised before the panel is ever shown and reassigned,
 * never cleared, so none of the defaults could fire — and if one ever did, it would fire in the one place
 * where a substituted value is a request that differs from the screen the operator is reading.
 *
 * The producer is the popup, which is the extension's own trusted page, so this is a DCHECK and not a refusal
 * — unlike lib/response-decode.js's arrival gate, whose sender is the page's renderer. What it asserts is
 * that the operator's CHOICES all arrived: a replay that quietly substituted a verb, dropped the header list
 * the operator typed, or forgot the content type they picked is a request that differs from the one on screen
 * in a way the screen cannot show them.
 *
 * `service` and `methodId` are `string | null` and the null is the point — "no discovery method was chosen"
 * is a state the Send panel is legitimately in, and the popup states it rather than omitting the field. */
function _checkSendPanelRequest(msg, command) {
  DCHECK(typeof msg.url === "string" && msg.url !== "",
         command + " arrived with no URL — the popup returns early rather than sending one without it, so " +
         "an absent URL here is that guard bypassed and the request would be built against nothing");
  DCHECK(typeof msg.httpMethod === "string" && msg.httpMethod !== "",
         command + " arrived with no HTTP method — it is the verb the operator selected, and substituting " +
         "one fires a different request than the panel is showing them");
  DCHECK(typeof msg.contentType === "string",
         command + " arrived with no content type — the popup holds one from the moment it loads and " +
         "replaces it from the schema or the captured request, so absence is that state broken; the empty " +
         "string is the panel legitimately naming none");
  DCHECK(typeof msg.headers === "object" && msg.headers !== null,
         command + " arrived with no header map — the popup builds one from the header rows on every send, " +
         "so `{}` is the operator having typed no headers and an absent map is those rows never read");
  DCHECK(msg.body === null || (typeof msg.body === "object" && typeof msg.body.mode === "string"),
         command + " arrived with a body that names no mode — every arm of the popup's body builder writes " +
         "one (form/graphql/multipart/raw) and a GET says `null`, so a mode-less object is an editor whose " +
         "contents no encoder will recognise");
  DCHECK(msg.service === null || typeof msg.service === "string",
         command + " arrived with a service that is neither a name nor a stated absence — `null` is \"no " +
         "discovery method chosen\", which is a state the panel is legitimately in and the consumer tests");
  DCHECK(msg.methodId === null || typeof msg.methodId === "string",
         command + " arrived with a methodId that is neither an id nor a stated absence — see `service`");
  DCHECK(!!msg.requestInit && typeof msg.requestInit === "object" &&
         ["credentials", "mode", "referrer", "referrerPolicy"].every((k) =>
           msg.requestInit[k] === null || typeof msg.requestInit[k] === "string"),
         command + " arrived with no requestInit — lib/popup-send.js's sendPanelRequestInit writes the four " +
         "members (null where unstated) on every send, and without them nobody chose the credentials mode");
  DCHECK(safeFetchStatedShape(msg.stated),
         command + " arrived with no stated {origin, referer, jar} — lib/popup-send.js's sendPanelStatedHeaders " +
         "writes all three (null for the frame's own) on every send");
  DCHECK(!("apiKeyOverride" in msg),
         command + " arrived with an apiKeyOverride — a key travels in the request's own header or query " +
         "parameter, where the panel shows it, and nothing here adds one");
}

/* A body is bytes when its type is a binary wire format; JSPB (json+protobuf) is text. */
function _isBinaryContentType(ct) {
  const lower = ct.toLowerCase();
  if (lower.includes("json")) return false;
  return lower.includes("proto") || lower.includes("grpc") || lower.includes("octet-stream") ||
         lower.startsWith("image/") || lower.startsWith("video/") || lower.startsWith("audio/") ||
         lower.includes("application/pdf") || lower.includes("application/zip");
}

/* An operator send: a request the person composed, sent from this zone as the frame `documentId` names
   (lib/request-identity.js's frameIdentity, browser-stamped), through safe-fetch.js's operator tier, which
   CHECKs that `sender` is an extension document. `opts` is {method, headers (a map), body (string or null),
   bodyEncoding (null or "base64"), requestInit, stated}, where `stated` is the Send panel's record
   (safeFetchStatedShape; PAGE_CONTEXT_STATED_NONE for a probe). Answers {error} or {ok, status, statusText,
   headers, body, bodyEncoding?}, where `body` is text or, for a binary type, base64.
   Not covered: the frame's own referrer policy is not observable from this zone, so a request that states
   none uses Referrer Policy §3's default. The next diff reads the policy the engine parsed when it loaded
   that document. Its absence shows as a Referer differing from the frame's own for a page that sets a policy. */
async function operatorSendAsFrame(sender, tabId, documentId, initiator, url, opts) {
  pageContextRequireUserInitiated(initiator, "operatorSendAsFrame (document " + documentId + ")");
  DCHECK(!!opts && typeof opts.method === "string" && opts.method !== "" && !!opts.headers &&
         (opts.body === null || typeof opts.body === "string") && !!opts.requestInit && !!opts.stated,
         "an operator send named no method, header map, body statement, requestInit or stated headers");
  if (!URL.canParse(url)) return { error: "blocked: invalid URL" };
  const who = await _operatorIdentity(tabId, documentId, opts.stated);
  if (who.error !== null) return { error: who.error };
  const { facts, mode } = await _operatorFacts(who.identity, url, opts.requestInit);
  const body = opts.body === null ? null : opts.bodyEncoding === "base64" ? base64ToUint8(opts.body) : opts.body;
  const r = await safeFetchOperator(url, { sender, identity: facts, method: opts.method, mode, body,
                                          headers: Object.keys(opts.headers).map((k) => [k, String(opts.headers[k])]),
                                          stated: opts.stated });
  if (r.refusal && r.refusal.reason.startsWith("forbidden-request-header:"))
    return { error: "refused: `" + r.refusal.reason.slice("forbidden-request-header:".length) + "` is a " +
                    "forbidden request-header (Fetch §2.2.2 Headers) — the browser sets it itself, so remove it " +
                    "from the request" };
  if (r.refusal && r.refusal.reason.startsWith("stated-origin-not-an-origin:"))
    return { error: "refused: the stated Origin is not a serialized http(s) origin (scheme://host[:port], or " +
                    "null), which is all an Origin header carries" };
  if (r.refusal && r.refusal.reason.startsWith("stated-referer-not-a-url:"))
    return { error: "refused: the stated Referer is not an absolute http(s) URL as the URL parser writes it, " +
                    "with no fragment" };
  if (r.refusal) return { error: r.refusal.kind + ": " + r.refusal.reason };
  const ct = r.headers["content-type"] || "";
  if (_isBinaryContentType(ct))
    return { ok: r.ok, status: r.status, statusText: r.statusText, headers: r.headers,
             body: uint8ToBase64(r.body), bodyEncoding: "base64" };
  return { ok: r.ok, status: r.status, statusText: r.statusText, headers: r.headers,
           body: new TextDecoder().decode(r.body) };
}

/* The facts an operator send as `identity` applies to `url`, and its logical mode: what the send and its
   preview both compute, so the preview cannot describe another request. */
async function _operatorFacts(identity, url, requestInit) {
  return { facts: await identityFor(identity, { url, requestInit }, "strict-origin-when-cross-origin"),
           mode: requestInit.mode === null ? "cors" : requestInit.mode };
}

/* Who an operator send speaks as: the frame `documentId` (frameIdentity, browser-stamped), with the cookie
   partition of the frame the person stated as the jar (`stated.jar`, itself resolved through frameIdentity),
   or its own. Answers {error} naming the frame that is gone, or {error: null, identity, frame, holder}:
   `identity` is the frame's origin and URL over the jar's partition key, `frame` the picked frame's own
   identity, and `holder` the identity of the frame whose jar it is. */
async function _operatorIdentity(tabId, documentId, stated) {
  const frame = await frameIdentity(tabId, documentId);
  if (frame === null) return { error: "the frame this request speaks as is gone (document " + documentId + ")" };
  if (stated.jar === null) return { error: null, identity: frame, frame, holder: frame };
  const holder = await frameIdentity(stated.jar.tabId, stated.jar.documentId);
  if (holder === null) return { error: "the frame whose cookie jar was picked is gone (document " +
                                       stated.jar.documentId + ")" };
  /* Its speak-as rules are scoped to the pair, and end with either document (lib/speak-as.js). */
  return { error: null, identity: { origin: frame.origin, docUrl: frame.docUrl, partitionKey: holder.partitionKey,
                                    docKey: frame.docKey + "+" + holder.docKey },
           frame, holder };
}

/* The jars and origins the Send panel offers: every http(s) frame open in any tab, as [{tabId, documentId,
   origin, url, partitionKey}], each partition key chrome.cookies.getPartitionKey's for that document. */
async function operatorJarDomain(sender) {
  CHECK(senderIsExtensionDocument(sender),
        "the jar domain was asked from " + JSON.stringify(sender && sender.origin) + " — it lists the frames " +
        "of every tab, which only an extension document may read");
  const out = [];
  for (const tab of await swRpc("tabs.query", {})) {
    // webNavigation's "null if the specified tab ID is invalid": the tab closed since the query.
    const frames = await swRpc("webNavigation.getAllFrames", { tabId: tab.id });
    if (frames === null) continue;
    for (const f of frames) {
      const id = await frameIdentityOf(f);
      if (id !== null) out.push({ tabId: tab.id, documentId: f.documentId, origin: id.origin, url: id.docUrl,
                                  partitionKey: id.partitionKey });
    }
  }
  return out;
}

/* The Send panel's identity preview: what an operator send of `method` to `url` as the frame `documentId`
   carries, by the calls operatorSendAsFrame makes, never their values for cookies. `stated` is the panel's
   record (safeFetchStatedShape). Answers lib/identity-preview.js's record: {error}, or the frame's
   browser-stamped identity, the jar and the frame holding it, the Origin (Fetch §3.2 "`Origin` header") and
   Referer (Referrer Policy §8.3) the frame's own send carries, the headers sent once the stated ones replace
   them, the cookie names with each excluded name's rule, and identityProducibleBy's answer. */
async function operatorIdentityPreview(sender, tabId, documentId, url, method, requestInit, stated) {
  CHECK(senderIsExtensionDocument(sender),
        "an identity preview was asked from " + JSON.stringify(sender && sender.origin) + " — it reads the " +
        "person's cookie names, which only an extension document may, as only one may send with them");
  if (!URL.canParse(url) || !/^https?:$/.test(new URL(url).protocol))
    return { error: "the address is not an http(s) URL, so the send path will refuse it" };
  const who = await _operatorIdentity(tabId, documentId, stated);
  if (who.error !== null) return { error: who.error };
  const { facts, mode } = await _operatorFacts(who.identity, url, requestInit);
  const origin = speakAsOrigin(facts.origin, method, mode, url, facts.referrerPolicy);
  const sent = { origin: stated.origin !== null ? stated.origin.header : origin,
                 referer: stated.referer !== null ? stated.referer : facts.referrer };
  const statedRefusal = safeFetchStatedRefusal(stated);
  const cookies = await cookieNamesFor(facts, url);
  const speaking = stated.origin !== null && stated.origin.header !== null ? stated.origin.header : who.frame.origin;
  const key = (k) => ({ topLevelSite: k.topLevelSite, hasCrossSiteAncestor: k.hasCrossSiteAncestor });
  const preview = {
    frame: { origin: who.frame.origin, docUrl: who.frame.docUrl, partitionKey: key(who.frame.partitionKey) },
    jar: { partitionKey: key(who.identity.partitionKey), source: stated.jar === null ? "frame" : "operator",
           holder: { origin: who.holder.origin, docUrl: who.holder.docUrl } },
    method, mode, origin, referer: facts.referrer, policy: facts.referrerPolicy,
    policySource: requestInit.referrerPolicy === null || requestInit.referrerPolicy === "" ? "default" : "request",
    referrerSource: requestInit.referrer === null || requestInit.referrer === "about:client" ? "client"
      : requestInit.referrer === "" ? "none" : "request",
    sent, statedRefusal, credentials: facts.credentials, cookieArm: cookies.arm, cookieNames: cookies.names,
    excluded: cookies.excluded,
    producibleBy: statedRefusal !== null ? null
      : await identityProducibleBy(who.identity, who.frame.partitionKey, facts, speaking, sent, method, mode, url),
  };
  checkIdentityPreview(preview, "operatorIdentityPreview");
  return preview;
}

/**
 * Execute a request from the Send panel.
 * Encodes form data, sends it as the observed frame (`operatorSendAsFrame`), decodes the response.
 */
async function executeSendRequest(documentId, msg, sender) {
  const startTime = Date.now();
  const service = msg.service;
  const methodId = msg.methodId;

  // Validate URL
  let parsedUrl;
  try {
    parsedUrl = new URL(msg.url);
    if (parsedUrl.protocol !== "http:" && parsedUrl.protocol !== "https:") {
      return { error: "blocked: invalid protocol" };
    }
  } catch (e) {
    // A URL the operator typed can legitimately fail to parse; an invariant abort inside this try would
    // otherwise be reported to them as that, i.e. as their own typing being at fault.
    RETHROW_FATAL(e);
    return { error: "invalid URL" };
  }

  // Build headers
  const headers = { ...msg.headers };
  if (
    msg.contentType !== "" &&
    msg.httpMethod !== "GET" &&
    msg.httpMethod !== "DELETE"
  ) {
    headers["Content-Type"] = msg.contentType;
  }

  /* No key is added here: the Send panel offers each learned key at the header or query parameter it was
     observed at (lib/popup-send.js renderKeyExamples), so a key is one of the bytes the operator sees. */
  const tab = _docForLearning(documentId);
  const tabId = (tab && tab.tabId != null) ? tab.tabId : msg.tabId; // the frame's tab, for frameIdentity; msg.tabId for a cross-tab replay

  const url = parsedUrl.toString();

  // Encode body
  let body = null;
  let bodyEncoding = null;

  if (msg.httpMethod !== "GET" && msg.httpMethod !== "DELETE" && msg.body) {
    // Check if this is a multipart batch sub-request (_batchPart methods)
    const _batchPartMethod = (() => {
      if (!service || !methodId) return null;
      const docEntry = tab.discoveryDocs.get(service) || globalStore.discoveryDocs.get(service);
      if (!docEntry?.doc) return null;
      const mName = methodId.split(".").pop();
      return docEntry.doc.resources?.learned?.methods?.[mName];
    })();

    if (_batchPartMethod?._batchPart && msg.body.mode === "form") {
      // Multipart batch: wrap form fields in a single-part multipart body
      const fields = msg.body.formData?.fields || [];
      const jsonBody = JSON.stringify(encodeFormToJson(fields));
      const partPath = _batchPartMethod.path;
      const partMethod = _batchPartMethod.httpMethod || "GET";
      const boundary = "batch_" + Date.now();
      body = `--${boundary}\r\nContent-Type: application/http\r\n\r\n` +
        `${partMethod} ${partPath} HTTP/1.1\r\n` +
        `Content-Type: application/json\r\nAccept: application/json\r\n\r\n` +
        jsonBody + `\r\n--${boundary}--`;
      headers["Content-Type"] = `multipart/mixed; boundary=${boundary}`;
    } else if (url.includes("batchexecute") && msg.body.mode === "form") {
      // Special handling for batchexecute: wrap in f.req envelope
      const fields = msg.body.formData?.fields || [];
      const argsArray = encodeFormToJspb(fields);
      const innerJson = JSON.stringify(argsArray);

      // Extract RPC ID from methodId (e.g. "Google.Photos.p1Takd" -> "p1Takd")
      const rpcId = methodId ? methodId.split(".").pop() : "unknown";

      const envelope = [[[rpcId, innerJson, null, "generic"]]];
      const params = new URLSearchParams();
      params.set("f.req", JSON.stringify(envelope));

      body = params.toString();
      headers["Content-Type"] =
        "application/x-www-form-urlencoded;charset=UTF-8";
    } else if (msg.body.mode === "raw" && msg.body.rawBody) {
      if (
        msg.contentType === "application/x-protobuf" ||
        msg.contentType === "application/grpc-web+proto" ||
        msg.contentType === "application/grpc-web-text+proto"
      ) {
        body = msg.body.rawBody;
        bodyEncoding = "base64";
      } else {
        body = msg.body.rawBody;
      }
    } else if (msg.body.mode === "form" && msg.body.formData?.fields?.length) {
      const fields = msg.body.formData.fields;
      if (
        msg.contentType === "application/grpc-web+proto" ||
        msg.contentType === "application/grpc-web-text+proto"
      ) {
        // gRPC-Web: encode protobuf, wrap in frame
        const pbBytes = encodeFormToProtobuf(fields);
        const framed = encodeGrpcWebFrame(pbBytes);
        body = uint8ToBase64(framed);
        bodyEncoding = "base64";
      } else if (msg.contentType === "application/x-protobuf") {
        const encoded = encodeFormToProtobuf(fields);
        body = uint8ToBase64(encoded);
        bodyEncoding = "base64";
      } else if (msg.contentType === "application/json+protobuf") {
        body = JSON.stringify(encodeFormToJspb(fields));
      } else if (msg.contentType.startsWith("application/x-www-form-urlencoded")) {
        // Form-urlencoded with f.req JSPB (non-batchexecute)
        const argsArray = encodeFormToJspb(fields);
        const params = new URLSearchParams();
        params.set("f.req", JSON.stringify(argsArray));
        body = params.toString();
      } else {
        body = JSON.stringify(encodeFormToJson(fields));
      }
    }
  }

  // GraphQL: wrap query/variables in standard envelope
  if (isGraphQLUrl(url) && msg.body !== null && msg.body.mode === "graphql") {
    body = encodeGraphQLBody(msg.body);
    headers["Content-Type"] = "application/json";
  }

  /* The Send panel's act: `extension/popup.js` binds `sendRequest` to the `btn-send` click, and the address,
     verb, headers and body came out of controls that person filled in and can see. It speaks as the frame the
     request was observed in, with the requestInit the panel states. A network error is an answer the person
     is shown; an invariant abort is not one. */
  let resp;
  try {
    resp = await operatorSendAsFrame(sender, tabId, documentId, PAGE_CONTEXT_USER_INITIATED, url,
                                     { method: msg.httpMethod, headers, body, bodyEncoding,
                                       requestInit: msg.requestInit, stated: msg.stated });
  } catch (err) {
    RETHROW_FATAL(err);
    return { error: `fetch_exception: ${err.message}`, timing: Date.now() - startTime };
  }

  const timing = Date.now() - startTime;
  if (resp.error) {
    return { error: resp.error, timing };
  }

  /* `operatorSendAsFrame` writes every field below on its success arm. The content-type lookup keeps its
     `|| ""` because a response legitimately carries none, and "" is what the sniffs below are written
     against. */
  const respCt = resp.headers["content-type"] || "";
  let bodyResult;

  if (isGrpcWeb(respCt)) {
    // gRPC-Web: pass raw bytes for frame-level rendering in popup
    try {
      let bytes;
      if (isGrpcWebText(respCt)) {
        bytes = base64ToUint8(
          resp.bodyEncoding === "base64" ? resp.body : btoa(resp.body),
        );
      } else {
        bytes = resp.bodyEncoding === "base64"
          ? base64ToUint8(resp.body)
          : new TextEncoder().encode(resp.body);
      }
      // Scan protobuf frames for keys
      const parsed = parseGrpcWebFrames(bytes);
      if (parsed) {
        for (const frame of parsed.frames) {
          if (frame.type !== "data") continue;
          try {
            pbDecodeTree(frame.data, 8, (val) => {
              if (typeof val === "string") {
                extractKeysFromText(documentId, val, url, "send_response_grpc");
              }
            });
          } catch (e) {
            /* One frame's protobuf decode failed — other frames in the
               same response still process. Surface so a malformed frame
               on an otherwise-valid response is visible.
               THE CALLBACK ABOVE RUNS `extractKeysFromText`, WHICH IS THE ENGINE-FACING HALF OF THIS ZONE, so
               this catch sits directly downstream of code that asserts. Without this line a DCHECK raised in
               there would be logged once at debug level as one bad protobuf frame, and the loop would carry
               on over the rest — an invariant abort turned into a per-frame note nobody reads. */
            RETHROW_FATAL(e);
            console.debug("[brain] send-response grpc-web frame decode failed:", e && e.message || e, "url=" + url);
          }
        }
      }
      // Serialize bytes as base64 array for message passing
      bodyResult = {
        format: "grpc_web",
        bytesB64: uint8ToBase64(bytes),
        raw: resp.body,
        size: bytes.length,
      };
    } catch (e) {
      /* Outer gRPC-Web frame parse failed — bytes weren't valid frame
         format. Fall back to binary blob so the reviewer still sees
         the raw response, but surface the parse failure so the format
         mismatch (likely a server bug or wrong content-type) is
         diagnosable. */
      RETHROW_FATAL(e);
      console.warn("[brain] send-response grpc-web parse failed:", e && e.message || e, "url=" + url);
      bodyResult = {
        format: "binary",
        parsed: null,
        raw: resp.body,
        size: resp.body.length,
      };
    }
  } else if (
    (resp.bodyEncoding === "base64" || isBinaryContentType(respCt)) &&
    (/^(image|video|audio)\//i.test(respCt) || /application\/(pdf|zip)/i.test(respCt))
  ) {
    // Non-API binary (media/document) — pass through for download
    const size = resp.bodyEncoding === "base64"
      ? Math.floor(resp.body.length * 3 / 4)
      : resp.body.length;
    bodyResult = {
      format: "binary_download",
      raw: resp.body,
      /* THE ABSENCE IS THE STATEMENT, SPELLED AS ONE. content.js writes `bodyEncoding` on its binary arm
         alone, so "not base64" IS "text" — asserted in lib/schema.js, which admits no third spelling. Written
         `|| "text"` this said the same word for a relay that declared text and for one that had stopped
         declaring anything, and the popup would have decoded the second as text on the strength of it. */
      bodyEncoding: resp.bodyEncoding === "base64" ? "base64" : "text",
      contentType: respCt,
      size,
    };
  } else if (resp.bodyEncoding === "base64" || isBinaryContentType(respCt)) {
    // Binary protobuf response
    try {
      const bytes =
        resp.bodyEncoding === "base64"
          ? base64ToUint8(resp.body)
          : new TextEncoder().encode(resp.body);
      const tree = pbDecodeTree(bytes, 8, (val) => {
        if (typeof val === "string") {
          extractKeysFromText(documentId, val, url, "send_response_protobuf");
        }
      });
      bodyResult = {
        format: "protobuf_tree",
        parsed: tree,
        raw: resp.body,
        size: bytes.length,
      };
    } catch (e) {
      /* A protobuf tree this decoder cannot walk is a DATUM about the bytes — the reviewer still gets the raw
         response. An invariant abort is not one and travels on, or a broken relay contract would be rendered
         as a server that answered unparseable protobuf. */
      RETHROW_FATAL(e);
      bodyResult = {
        format: "binary",
        parsed: null,
        raw: resp.body,
        size: resp.body.length,
      };
    }
  } else {
    // Try JSON parse (strip Google XSSI prefix if present)
    let jsonText = resp.body;
    if (jsonText.trimStart().startsWith(")]}'")) {
      jsonText = jsonText.trimStart().substring(4).trimStart();
    }
    try {
      const parsed = JSON.parse(jsonText);
      if (
        Array.isArray(parsed) &&
        (respCt.includes("json+protobuf") ||
          (respCt.includes("text/plain") &&
            parsed.length > 0 &&
            parsed.some((item) => item === null || Array.isArray(item) || typeof item !== "object")) ||
          (respCt.includes("json") &&
            parsed.length > 0 &&
            parsed.some((item) => item === null || Array.isArray(item) || typeof item !== "object")))
      ) {
        // JSPB format: json+protobuf content-type, or text/plain/json with array structure
        bodyResult = {
          format: "protobuf_tree",
          parsed: jspbToTree(parsed),
          raw: resp.body,
          size: resp.body.length,
          isJspb: true,
        };
      } else {
        bodyResult = {
          format: "json",
          parsed,
          raw: resp.body,
          size: resp.body.length,
        };
      }
    } catch (e) {
      /* "It is not JSON" is a FACT ABOUT THE RESPONSE and the text arm is the right answer to it. An invariant
         abort is not, and without this line a broken relay record would be rendered to the reviewer as a
         server that returned plain text. */
      RETHROW_FATAL(e);
      bodyResult = {
        format: "text",
        parsed: null,
        raw: resp.body,
        size: resp.body.length,
      };
    }
  }

  /* `null` IS THE ABSENCE, IN THE SPELLING THE REST OF THIS RECORD ALREADY USES. A Map miss answers
     `undefined`, and this record crosses chrome.runtime.sendMessage to the popup — a serialization that DROPS
     an undefined property, so "this document knows no discovery doc for that service" arrived as a record
     with no `discovery` KEY AT ALL, which is byte-identical to this producer having stopped writing one.
     `service` and `methodId` beside it already say "nothing was chosen" with null and are asserted as such
     where the command arrives; this is the third member of that trio and it now says it the same way. */
  const _discoveryHit = tab.discoveryDocs.get(msg.service);
  const discovery = _discoveryHit === undefined ? null : _discoveryHit;

  return {
    ok: resp.ok,
    status: resp.status,
    /* `""` IS A REAL ANSWER HERE, which is exactly why it may not double as a missing one: an HTTP/2 response
       carries no reason phrase, so `|| ""` said the same two bytes for that server and for a relay that had
       stopped writing the field. lib/schema.js asserts the string exists; this passes on whichever one it is.
       (extension/bridge.js's `fetchedXhr` states the same rule for the same reason.) */
    statusText: resp.statusText,
    headers: resp.headers,
    body: bodyResult,
    timing,
    discovery, // Pass back latest doc (+ summary/apiKey)
    service, // Echo back metadata
    methodId,
    error: null,
  };
}

// ─── Helpers ─────────────────────────────────────────────────────────────────
