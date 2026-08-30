/* Discovery -> OpenAPI 3.0 exporter — extracted from discovery.js (classic script, globals; loaded in the
   worker AND popup). Converts the internal discovery/VDD format to an OpenAPI 3.0 spec for the popup's
   Export feature: discoveryTypeToJsonSchema + discoverySchemaToOpenApi + convertDiscoveryToOpenApi. */
// ─── VDD → OpenAPI 3.0 Export ───────────────────────────────────────────────

/**
 * Map internal protobuf-style type to JSON Schema type + format.
 */
function discoveryTypeToJsonSchema(type) {
  switch (type) {
    case "string": return { type: "string" };
    case "bytes": return { type: "string", format: "byte" };
    case "bool": return { type: "boolean" };
    case "int32": case "uint32": return { type: "integer", format: type };
    case "int64": case "uint64": return { type: "string", format: type };
    case "float": return { type: "number", format: "float" };
    case "double": case "number": return { type: "number", format: "double" };
    case "enum": return { type: "string" };
    case "array": return { type: "array" };
    case "any": return {};
    default: return { type: "string" };
  }
}

/* A RENAME IS A PAIR, AND READING HALF OF IT IS WHY A BOOLEAN COULD BECOME A FIELD NAME.
 *
 * `name` on a stored schema property or discovery parameter is written by exactly one thing: the reviewer
 * renaming it in the popup (lib/popup-handlers.js's RENAME_FIELD), which writes `name` and `customName`
 * TOGETHER, and lib/serialize.js carries the two together across every re-derivation so an automated answer
 * cannot clobber a manual edit. So the field is LEGITIMATELY ABSENT, and its absence is a POSITIVE
 * STATEMENT — nobody renamed this, therefore the map KEY is the name — with `customName` as the flag that
 * says which of the two states holds. lib/discovery.js and lib/send.js both already read it as that pair.
 *
 * `prop.name || prop.customName || key` read it as neither. `customName` is a BOOLEAN at every writer, so
 * the middle operand could only ever contribute the literal `true` — an exported OpenAPI document with a
 * property named "true" in it, produced by the one arm that fires when a rename is recorded and its name is
 * not. It is the defaulted-read defect at its most literal: the `||` chain does not tolerate an absence, it
 * manufactures a datum out of a flag about a different question.
 *
 * HOW ITS ABSENCE WOULD SHOW: a spec exported for a service whose schema was renamed carries a property the
 * server has never heard of, and an import of that spec re-learns it as a real field.
 */
function _renamedTo(rec, key, where) {
  if (rec.customName !== true) return key;                 // never renamed — the key IS the name
  DCHECK(typeof rec.name === "string" && rec.name !== "",
         "a rename was recorded for " + where + " with no replacement name — RENAME_FIELD writes `name` and " +
         "`customName` in one statement and serialize.js carries them as a pair, so half a rename is a " +
         "producer broken between them, and the name it would export is a fact about neither the server nor " +
         "the reviewer");
  return rec.name;
}

/**
 * Convert a Discovery schema object to OpenAPI 3.0 schema.
 * @param {object} schema - Discovery schema (id, type, properties, required)
 * @param {object} allSchemas - All schemas in the doc (for $ref resolution)
 * @param {Set} visited - Circular reference guard
 * @returns {object} OpenAPI schema object
 */
function discoverySchemaToOpenApi(schema, allSchemas, visited) {
  if (!schema) return { type: "object" };
  if (!visited) visited = new Set();
  if (visited.has(schema.id)) return { $ref: "#/components/schemas/" + schema.id };
  visited.add(schema.id);

  const result = { type: "object", properties: {} };
  const required = [];

  if (schema.properties) {
    for (const [key, prop] of Object.entries(schema.properties)) {
      const fieldName = _renamedTo(prop, key, "the schema property " + schema.id + "." + key);

      if (prop.type === "message" || prop.$ref) {
        const refName = prop.$ref || prop.messageType;
        if (refName && allSchemas[refName]) {
          if (prop.label === "repeated") {
            result.properties[fieldName] = {
              type: "array",
              items: { $ref: "#/components/schemas/" + refName },
            };
          } else {
            result.properties[fieldName] = { $ref: "#/components/schemas/" + refName };
          }
        } else {
          result.properties[fieldName] = { type: "object" };
        }
      } else if (prop.label === "repeated" || prop.type === "array") {
        const itemType = prop.items?.$ref
          ? { $ref: "#/components/schemas/" + prop.items.$ref }
          : prop.items?.type
            ? discoveryTypeToJsonSchema(prop.items.type)
            : { type: "string" };
        result.properties[fieldName] = { type: "array", items: itemType };
      } else {
        const ts = discoveryTypeToJsonSchema(prop.type || "string");
        result.properties[fieldName] = { ...ts };
        if (prop.enum) result.properties[fieldName].enum = prop.enum;
      }

      if (prop.description) result.properties[fieldName].description = prop.description;
      if (prop.number) {
        result.properties[fieldName]["x-field-number"] = prop.number;
      }
      if (prop.required) required.push(fieldName);
    }
  }

  if (required.length) result.required = required;
  visited.delete(schema.id);
  return result;
}

/**
 * Convert a Discovery document to OpenAPI 3.0.3 spec.
 * @param {object} doc - Discovery doc (from discoveryDocs Map)
 * @param {string} serviceName - Service key (e.g. "people.googleapis.com")
 * @returns {object} OpenAPI 3.0.3 JSON object
 */
function convertDiscoveryToOpenApi(doc, serviceName) {
  const spec = {
    openapi: "3.0.3",
    info: {
      title: doc.title || serviceName,
      description: doc.description || "Exported from UASR",
      version: doc.version || "v1",
      // Preserve the internal service key (hostname + path prefix) so
      // roundtrip import keys back to the SAME service. The import-side
      // fallback derives hostname-only from servers[0].url, which loses
      // path-prefixed keys like "www.google.com/MapsWizUi".
      "x-service-key": serviceName,
    },
    servers: [{ url: doc.rootUrl || doc.baseUrl || "https://" + serviceName }],
    paths: {},
    components: { schemas: {} },
  };

  // Convert schemas
  if (doc.schemas) {
    for (const [name, schema] of Object.entries(doc.schemas)) {
      spec.components.schemas[name] = discoverySchemaToOpenApi(
        schema, doc.schemas, new Set(),
      );
    }
  }

  // Convert auth
  if (doc.auth?.oauth2?.scopes) {
    spec.components.securitySchemes = {
      oauth2: {
        type: "oauth2",
        flows: {
          implicit: {
            authorizationUrl: "https://accounts.google.com/o/oauth2/auth",
            scopes: {},
          },
        },
      },
    };
    for (const [scope, def] of Object.entries(doc.auth.oauth2.scopes)) {
      spec.components.securitySchemes.oauth2.flows.implicit.scopes[scope] =
        def.description || "";
    }
  }
  if (doc.auth?.apiKey) {
    if (!spec.components.securitySchemes) spec.components.securitySchemes = {};
    spec.components.securitySchemes.apiKey = {
      type: "apiKey",
      name: doc.auth.apiKey.name || "key",
      in: doc.auth.apiKey.in || "query",
    };
  }

  // Walk resources to extract methods
  function walkResources(resources, prefix) {
    if (!resources) return;
    for (const [rName, resource] of Object.entries(resources)) {
      if (resource.methods) {
        for (const [methodName, method] of Object.entries(resource.methods)) {
          const basePath = "/" + (method.path || "").replace(/^\/?/, "");
          const httpMethod = (method.httpMethod || "POST").toLowerCase();

          // OpenAPI 3.x allows only one operation per (path, verb). Multiple
          // GraphQL operations share the same POST /graphql path, as do
          // many RPC-style endpoints (batchexecute, gRPC-Web). Synthesize a
          // unique path-level discriminator so every learned method gets
          // its own OpenAPI entry. Readers that don't care about the
          // discriminator still see the real path via `x-original-path`.
          let path = basePath;
          const existing = spec.paths[path] && spec.paths[path][httpMethod];
          if (existing && existing.operationId !== method.id) {
            // Collision — suffix with the method's logical name.
            path = basePath + (basePath.includes("?") ? "&" : "?") + "x-operation=" + encodeURIComponent(methodName);
          }
          if (!spec.paths[path]) spec.paths[path] = {};

          const operation = {
            operationId: method.id,
            description: method.description || "",
            parameters: [],
            responses: { "200": { description: "OK" } },
          };

          // Parameters. OpenAPI 3.0 `in` accepts only query/header/path/cookie;
          // form-body params (location:"body" learned from form-urlencoded
          // POSTs) must live in requestBody instead or the import path will
          // silently drop them.
          const formBodyProps = {};
          const formBodyRequired = [];
          const formBodyFieldNumbers = {};
          if (method.parameters) {
            for (const [pName, pDef] of Object.entries(method.parameters)) {
              const paramName = _renamedTo(pDef, pName, "the parameter " + rName + "." + methodName + "." + pName);
              /* A PARAMETER WHOSE DOCUMENT STATES NO TYPE IS A STRING PARAMETER, and that is a fact about
                 the document rather than a hole this line fills — lib/discovery.js reaches the same "string"
                 by the same reasoning for a property whose document says nothing else. What the `||` also
                 did, and could not distinguish, was swallow a `type` that is not a string at all: these
                 records mix OUR writers (lib/learn.js states a type on every parameter it mints) with a
                 Google Discovery document FETCHED from the target's server, so a non-string here is
                 third-party bytes and is REFUSED rather than asserted — a DCHECK on them would be the
                 trusted zone aborting on input somebody else chose (lib/field-def.js states that split). */
              const pType = fdDocString(pDef.type);
              const paramSchema = {
                type: pType === null ? "string" : pType,
                ...(pDef.enum ? { enum: pDef.enum } : {}),
                ...(pDef.format ? { format: pDef.format } : {}),
                ...(pDef._defaultValue != null ? { default: pDef._defaultValue } : {}),
                /* THE OBSERVED SPAN IS NOT A SCHEMA ASSERTION AND MUST STOP BEING WRITTEN AS ONE.
                   `_range` is the min and max of the values live traffic HAPPENED TO CARRY (lib/learn.js's
                   analyzeRange over fieldStats). Written as §6.2.4 "minimum" / §6.2.2 "maximum" it asserts
                   that the API REJECTS anything outside, which no run of anything ever observed — a domain
                   claim manufactured out of a sample, and the same defect §@H names when it forbids inventing
                   `6` for `x > 5`. It also collided head-on with the line below, which writes those two
                   keywords from a bound the code actually stated: two producers, one keyword, last spread
                   wins, and nothing to say which claim the reader is looking at.
                   The datum is KEPT, as an OAS 3.0.3 Specification Extension (`^x-` on a Schema Object),
                   where it annotates without validating. The popup renders it under its own "range" badge,
                   which already says "the domain this field's observations spanned". */
                ...(pDef._range ? { "x-observed-range": { min: pDef._range.min, max: pDef._range.max } } : {}),
                /* AND THE ORDERING GATE'S INTERVAL, in the standard's own vocabulary — which is also the
                   vocabulary endpoint.c emitted and learn.js merged, so this is a spread and not a rename.
                   JSON Schema Validation 2020-12 §6.2 Validation Keywords for Numeric Instances (number and
                   integer): §6.2.2 "maximum" ("an inclusive upper limit for a numeric instance"), §6.2.3
                   "exclusiveMaximum" ("strictly less than (not equal to)"), §6.2.4 "minimum" ("an inclusive
                   lower limit"), §6.2.5 "exclusiveMinimum" ("strictly greater than (not equal to)"). All four
                   take a NUMBER in 2020-12; the draft-04 boolean form is a different keyword and would be
                   read here as a bound of 1.
                   It states the interval and never a member of it — §@H's line between a domain and an
                   invented value — so `enum`/`default` stay driven by values the code COMPUTED. Emitted only
                   where the claim survived every observed path; lib/learn.js writes `null` where another path
                   disproved it, which is why this is a presence test and not a `||`. */
                ...(pDef._bounds && typeof pDef._bounds === "object" ? { ...pDef._bounds } : {}),
                /* THE DOMAIN THE CODE'S OWN GATES STATED, in the standard's own vocabulary for it.
                   JSON Schema Core 2020-12 §10.2.1.4 "not": "An instance is valid against this keyword if it
                   fails to validate successfully against the schema defined by this keyword"; JSON Schema
                   Validation 2020-12 §6.1.2 "enum": "An instance validates successfully against this keyword
                   if its value is equal to one of the elements in this keyword's array value". Composed, they
                   say exactly what the forced execution proved: any value except these.
                   IT IS NOT `enum`, WHICH ASSERTS THE OPPOSITE — that these are the only values the parameter
                   takes — and that inversion is the one way this export could turn an observation into a lie.
                   OAS 3.0.3 Schema Object carries both keywords, with `not` required to be a Schema Object
                   rather than a standard JSON Schema, which `{enum:[…]}` is.
                   Emitted only where a constraint held on every observed path; the absence is the statement,
                   matching endpoint.c's own omission rule. */
                ...(Array.isArray(pDef._excludedValues) && pDef._excludedValues.length
                    ? { not: { enum: pDef._excludedValues.slice() } } : {}),
              };
              /* NO DEFAULT. `endpoint.c` emits a `location` per param and `learn.js` writes it onto every
                 parameters entry, so an absent one means a record older than that producer — a stale IDB
                 entry, which must say so rather than be exported as a query param it may not be. */
              DCHECK(pDef.location === "path" || pDef.location === "query" || pDef.location === "body" ||
                     pDef.location === "form" || pDef.location === "formData",
                     "a discovery parameter reached the OpenAPI export with location " + pDef.location +
                     " — endpoint.c states path/query/body and openapi-import.js writes p.in; an absent one is " +
                     "a record persisted before that producer existed");
              const loc = pDef.location;
              if (loc === "body" || loc === "form" || loc === "formData") {
                // Form-body params → accumulate into requestBody schema.
                formBodyProps[paramName] = paramSchema;
                if (pDef.required) formBodyRequired.push(paramName);
                if (pDef.number) formBodyFieldNumbers[paramName] = pDef.number;
                continue;
              }
              /* `description` IS OPTIONAL IN THE PARAMETER OBJECT, so a parameter nothing described is
                 exported WITHOUT the key — omission is the document making no claim, while `""` is the
                 document claiming the description is empty, and only one of those is true. Same refusal as
                 `type`: our own writers state a description on every parameter they mint, and a non-string
                 one comes off a fetched Discovery document, so it is refused rather than asserted. */
              const pDesc = fdDocString(pDef.description);
              const param = {
                name: paramName,
                in: loc,
                required: !!pDef.required,
                ...(pDesc === null ? {} : { description: pDesc }),
                schema: paramSchema,
              };
              if (pDef.number) param["x-field-number"] = pDef.number;
              if (pDef._requiredConfidence != null) param["x-observed-frequency"] = pDef._requiredConfidence;
              operation.parameters.push(param);
            }
          }

          // Request body: prefer schema $ref; otherwise, synthesize a
          // form-urlencoded schema from accumulated body params so a
          // roundtrip through import can recover them.
          if (method.request?.$ref && doc.schemas?.[method.request.$ref]) {
            const ct = "application/json";
            operation.requestBody = {
              content: {
                [ct]: { schema: { $ref: "#/components/schemas/" + method.request.$ref } },
              },
            };
          } else if (Object.keys(formBodyProps).length > 0) {
            const schema = {
              type: "object",
              properties: formBodyProps,
            };
            if (formBodyRequired.length) schema.required = formBodyRequired;
            if (Object.keys(formBodyFieldNumbers).length) {
              schema["x-field-numbers"] = formBodyFieldNumbers;
            }
            operation.requestBody = {
              content: { "application/x-www-form-urlencoded": { schema } },
            };
          }

          // Response body
          if (method.response?.$ref && doc.schemas?.[method.response.$ref]) {
            operation.responses["200"].content = {
              "application/json": {
                schema: { $ref: "#/components/schemas/" + method.response.$ref },
              },
            };
          }

          // Scopes
          if (method.scopes?.length) {
            operation.security = [{ oauth2: method.scopes }];
          }

          // Chain data
          if (method._chains) {
            var chainExport = {};
            if (method._chains.incoming?.length) {
              chainExport.incoming = method._chains.incoming.map(function(c) {
                return { param: c.paramName, from: c.sourceMethodId + "." + c.sourceFieldPath, count: c.observedCount || 1 };
              });
            }
            if (method._chains.outgoing?.length) {
              chainExport.outgoing = method._chains.outgoing.map(function(c) {
                return { field: c.sourceFieldPath, to: c.targetMethodId + "." + c.paramName, count: c.observedCount || 1 };
              });
            }
            if (chainExport.incoming || chainExport.outgoing) {
              operation["x-data-chains"] = chainExport;
            }
          }

          // Preserve the original path so importers can roundtrip back
          // without the x-operation discriminator.
          if (path !== basePath) operation["x-original-path"] = basePath;

          // When multiple discovery buckets carry the same method id
          // (learned.X and probed.X for the same endpoint) the second
          // write would overwrite the first and lose its parameters.
          // Instead, MERGE: union parameters by name, prefer non-empty
          // fields, keep request/response schema when either bucket has
          // one. This preserves the information from both sources.
          const priorOp = spec.paths[path][httpMethod];
          if (priorOp && priorOp.operationId === operation.operationId) {
            // Merge params
            const mergedParams = [];
            const seenParamNames = new Set();
            for (const src of [priorOp.parameters || [], operation.parameters]) {
              for (const p of src) {
                if (!seenParamNames.has(p.name)) {
                  seenParamNames.add(p.name);
                  mergedParams.push(p);
                }
              }
            }
            operation.parameters = mergedParams;
            // Prefer prior description if new is empty.
            if (!operation.description && priorOp.description) {
              operation.description = priorOp.description;
            }
            // Keep requestBody/responses where present.
            if (!operation.requestBody && priorOp.requestBody) operation.requestBody = priorOp.requestBody;
            if (priorOp.responses) {
              operation.responses = Object.assign({}, priorOp.responses, operation.responses);
            }
            // Preserve x-data-chains if prior had them
            if (!operation["x-data-chains"] && priorOp["x-data-chains"]) {
              operation["x-data-chains"] = priorOp["x-data-chains"];
            }
          }

          if (!operation.parameters.length) delete operation.parameters;

          spec.paths[path][httpMethod] = operation;
        }
      }
      if (resource.resources) {
        walkResources(resource.resources, prefix + rName + ".");
      }
    }
  }

  walkResources(doc.resources, "");

  return spec;
}
