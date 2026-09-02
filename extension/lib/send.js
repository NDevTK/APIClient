// lib/send.js — Send-panel replay backend: resolve an endpoint's request schema, coerce the form field values
// to their declared types, and EXECUTE the request in the target page's context (PAGE_FETCH relay -- cookies +
// session state attached). Extracted from the offscreen-brain.js monolith (one problem per file); loaded before
// it, resolves the encode fns (lib/encode.js) + pageContextSend at call-time. The manual testing workbench.

/* A PROBE FIELD IS A DIFFERENT RECORD FROM A FieldDef, AND IT IS TRANSLATED RATHER THAN SPREAD. lib/
   req2proto.js composes it by reading a `google.rpc.Status` the SERVER wrote, and it carries names the Send
   panel has never rendered (`wellKnown`, `package`, `messageName`, `requiredChildren`, `isEnum`) beside the
   ones it has — so `{...probeField}` would smuggle a second vocabulary into the panel's record, which is the
   disagreement lib/field-def.js exists to end. It is translated field by field, refusing each value the way
   any third-party document's value is refused (the bytes are a server's, and lib/persistence.js round-trips
   them through IndexedDB before they reach here).
   ITERATIVE, because a probe's nesting depth is the SERVER's choice: req2proto attaches children from nested
   probes with no depth of its own, so a self-recursive translator would be a stack whose bound an origin
   server sets. */
function _probeFieldsToDefs(fieldsObj) {
  function one(name, raw) {
    const pf = fdDocRecord(raw) === null ? {} : raw;
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
  const entries = (fieldsObj && typeof fieldsObj === "object") ? Object.entries(fieldsObj) : [];
  for (const [k, raw] of entries) {
    /* THE KEY IS THE NAME WHERE THE PROBE STORE IS AN OBJECT, and the record's own `name` where it is an
       array (lib/req2proto.js pushes, lib/discovery-probe.js keys) — a prefer-A-else-B alternative between
       two producers, not a hole: an array index is not a wire key and must never be rendered as one. */
    const named = Array.isArray(fieldsObj) ? fdDocString(raw && raw.name) : k;
    const built = one(named === null ? k : named, raw);
    out.push(built.def);
    if (built.kids) queue.push({ into: built.def.children, kids: built.kids });
  }
  while (queue.length > 0) {
    const item = queue.shift();
    for (const raw of item.kids) {
      const nm = fdDocString(raw && raw.name);
      const built = one(nm === null ? "" : nm, raw);
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
  const ep = _epHit === undefined ? null : _epHit;
  if (ep) checkEndpointRecord(ep, "lib/send.js resolving a Send-panel schema, key " + JSON.stringify(endpointKey));

  // If no endpoint but we have service+methodId (virtual), create a dummy ep object for context
  if (!ep && (!service || !methodId)) return { source: "none", endpoint: null };

  /* THE ENDPOINT'S OWN SERVICE WHEN THERE IS AN ENDPOINT, the caller's argument only when there is none.
     `ep?.service || service` collapsed those into one expression whose two arms answer different questions,
     and its reachable effect was to answer a captured endpoint's discovery lookup with whatever service the
     panel happened to have selected — a lookup against a doc this address was never filed under. */
  const targetService = ep ? ep.service : service;

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
    } else if (ep) {
      // Path matching (captured endpoint)
      /* THE VERB IS ON THE RECORD. `|| "POST"` matched this address against the discovery doc's POST method
         whenever the endpoint's own verb went missing — a GET endpoint resolved to a POST method's schema,
         which the Send panel then renders as this endpoint's parameters. lib/merge.js asserts the call site
         carries a method before it registers one, so there is no absence here to answer for. */
      match = findDiscoveryMethod(doc, ep.path, ep.method);
    }

    if (match) {
      source = "discovery";
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
  const probeResult = endpointKey
    ? globalStore.probeResults.get(endpointKey)
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
  if (probeResult?.probeDetails) {
    for (const pd of probeResult.probeDetails) {
      if (
        pd.fieldCount > 0 &&
        pd.contentType &&
        !contentTypes.includes(pd.contentType)
      ) {
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
                                              JSON.stringify(endpointKey)) : null;
  if (_epHoles && _epHoles.size) {
    parameters = parameters || {};
    for (const [_hn, _hp] of _epHoles) {
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
      const cur = declared ? parameters[_hn]
                           : { name: _hn, customName: false, number: null, type: "string", location: "path",
                               required: true, description: "AST-learned path segment",
                               format: null, enum: null,
                               _requiredConfidence: null, _detectedEnum: false,
                               _defaultValue: null, _defaultConfidence: null, _range: null,
                               _exampleValue: null, _exampleValueSource: null,
                               _excludedValues: null, _bounds: null, _predicates: null,
                               _looselyEquals: null,
                               _astForcedValues: null,
                               _astValidValues: null };
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
      DCHECK((priorVals === null || Array.isArray(priorVals)) &&
             (priorForced === null || Array.isArray(priorForced)),
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
      const _f = foldValuePools(priorVals === null ? [] : priorVals,
                                priorForced === null ? [] : priorForced, _hp.valid, _hp.forced);
      /* AN EMPTY POOL IS WRITTEN `null`, WHICH IS THIS MAP'S ONE SPELLING OF "nothing was learned at this
         grade" — the same one both literals above declare. A forced-only hole therefore leaves
         `_astValidValues: null` rather than `[]`, and there is nothing for the prefill below to take, which
         is exactly right: §@H permits a domain-annotated shape with no example and forbids inventing one. */
      cur._astValidValues = _f.valid.length ? _f.valid : null;
      cur._astForcedValues = _f.forced.length ? _f.forced : null;
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
}

/**
 * Execute a request from the Send panel.
 * Encodes form data, sends via pageContextSend, decodes response.
 */
async function executeSendRequest(documentId, msg) {
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

  // API key: user override → service keys → discovery doc key. (The endpoint record itself carries no key:
  // lib/merge.js, its only producer, writes none, so the lookup that stood here was fetching a record to
  // read two fields off it that do not exist.)
  const tab = _docForLearning(documentId);
  const tabId = (tab && tab.tabId != null) ? tab.tabId : msg.tabId; // Chrome routing for pageContextSend — the doc's tab; fall back to msg.tabId for cross-tab replay
  let apiKey = null;
  let apiKeySource = "header";

  if (msg.apiKeyOverride) {
    // User explicitly selected a key (or disabled injection) from the Send panel
    if (msg.apiKeyOverride.disabled) {
      apiKey = null; // Skip all auto-selection
    } else {
      apiKey = msg.apiKeyOverride.key || null;
      apiKeySource = msg.apiKeyOverride.source || "header";
    }
  }
  /* NO `ep.apiKey` / `ep.apiKeySource` ARM. An endpoint record has neither field (lib/merge.js is its only
     producer), so this branch read undefined twice and resolved to exactly the initial values above — the
     service-wide key search below is what has always found the key. */

  if (!msg.apiKeyOverride && !apiKey && service) {
    const hostname = parsedUrl.hostname;
    const svcKeys = collectKeysForService(tab, service, hostname);
    // Also check globalStore for keys from previous sessions
    if (svcKeys.length === 0) {
      for (const [key, data] of globalStore.apiKeys) {
        if (data.services?.has(service) || data.hosts?.has(hostname)) {
          svcKeys.push(key);
        }
      }
    }
    if (svcKeys.length > 0) {
      // Use the first matching key. We do NOT tiebreak by "same origin as the
      // current tab": mapping a tabId to an origin assumes the main frame and races
      // navigation (banned). The keys are already filtered to this service/host.
      apiKey = svcKeys[0];
      // Look up the actual location (url vs specific header name) the key
      // was originally observed in — keys captured from
      // `X-Goog-Api-Key` shouldn't be re-emitted as Google-branded
      // headers against non-Google targets like statsigapi.
      if (apiKey) {
        var _skStoredData = tab.apiKeys.get(apiKey) || globalStore.apiKeys.get(apiKey);
        if (_skStoredData && _skStoredData.source) {
          apiKeySource = _skStoredData.source;
        } else {
          apiKeySource = null; // unknown origin — don't guess a header name
        }
      }
    }
    // Fall back to discovery doc's key
    if (!apiKey) {
      const docEntry = tab.discoveryDocs.get(service) || globalStore.discoveryDocs.get(service);
      if (docEntry?.apiKey) apiKey = docEntry.apiKey;
    }
  }

  // Only add key if not already present in headers or URL
  const hasKeyHeader = headers["X-Goog-Api-Key"] || headers["x-goog-api-key"];
  const hasKeyParam = parsedUrl.searchParams.has("key");
  if (apiKey && !hasKeyHeader && !hasKeyParam) {
    // apiKeySource carries either "url", "header:<name>", or a legacy
    // "header" (no name). Only inject when we know the exact location —
    // silently defaulting to X-Goog-Api-Key for arbitrary third-party
    // hosts pollutes their requests with a Google-branded header that
    // the server doesn't recognize. Fall back to X-Goog-Api-Key only for
    // Google-ish hostnames where it is the genuine convention.
    if (apiKeySource === "url") {
      parsedUrl.searchParams.set("key", apiKey);
    } else if (typeof apiKeySource === "string" && apiKeySource.startsWith("header:")) {
      var _hdrName = apiKeySource.slice("header:".length);
      headers[_hdrName] = apiKey;
    } else if (/\.google(?:apis)?\.com$/i.test(parsedUrl.hostname) || /\.clients6\.google\.com$/i.test(parsedUrl.hostname)) {
      headers["X-Goog-Api-Key"] = apiKey;
    }
    // Otherwise skip auto-attach — let the user pick explicitly via the
    // Send panel's key selector if they want to try a specific key.
  }

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

  // Send request via page context (session-aware)
  let resp;
  try {
    resp = await pageContextSend(
      tabId,
      url,
      {
        method: msg.httpMethod,
        headers,
        body,
        bodyEncoding,
      },
      documentId,
      /* THE GRADE, AND THIS IS THE CALL IT WAS WRITTEN FOR. lib/schema.js's page-context relay is scoped out
         of the credentialed destructive-path deny list because a human composed what it carries, and the Send
         panel is that claim in its strongest form: `extension/popup.js` binds `sendRequest` to the `btn-send`
         click, and every field of this request — the address, the verb, the headers and the body — came out of
         controls that person filled in and can see. This is the one path where a token list refusing on a
         substring would be the tool vetoing its operator. */
      PAGE_CONTEXT_USER_INITIATED,
    );
  } catch (err) {
    /* AN INVARIANT ABORT IS NOT A FAILED REQUEST. `pageContextSend` and the `pageContextFetch` under it each
       DCHECK that the caller named an HTTP method, and a DCHECK is a throw on this side (extension/check.js);
       without this line a broken relay contract would be reported to the reviewer as `fetch_exception`, i.e.
       as the server or the network having refused — the one reading in which the user has no reason to look
       at this extension. Everything else a relay throw can be is still handled as the catch intends. */
    RETHROW_FATAL(err);
    return { error: `fetch_exception: ${err.message}`, timing: Date.now() - startTime };
  }

  const timing = Date.now() - startTime;

  /* THE RELAY ALWAYS ANSWERS WITH AN OBJECT. lib/schema.js's `pageContextFetch` returns {error} for a bad
     URL, {error} for an unreachable content script, and the content script's own reply otherwise — there is
     no arm on which it resolves undefined. `resp?.error || "fetch_failed: no response"` therefore carried a
     third case that cannot happen, and a `?.` over a guaranteed object is how a relay that started
     answering nothing would be reported as a request that simply failed. */
  DCHECK(resp && typeof resp === "object",
         "the page-context relay resolved without a response object — lib/schema.js's pageContextFetch " +
         "answers {error} on every failure arm, so an absent object is that relay broken rather than a " +
         "request the page refused");
  if (resp.error) {
    return { error: resp.error, timing };
  }

  /* EVERY FIELD READ BELOW IS ONE lib/schema.js's `_checkPageFetchReply` HAS ALREADY PROVEN EXISTS, which is
     why none of them is read through a default any more. `resp.headers` is an object, so the `?.` is gone; the
     KEY lookup keeps its `|| ""` because a response legitimately carries no content-type and "" is what the
     sniffs below are written against — an absent HEADER is a fact about the server, an absent `headers` was a
     fact about this extension, and only the second was ever being concealed. */
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
