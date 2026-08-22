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
      const fieldName = prop.name || prop.customName || key;

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
              const paramName = pDef.name || pName;
              const paramSchema = {
                type: pDef.type || "string",
                ...(pDef.enum ? { enum: pDef.enum } : {}),
                ...(pDef.format ? { format: pDef.format } : {}),
                ...(pDef._defaultValue != null ? { default: pDef._defaultValue } : {}),
                ...(pDef._range ? { minimum: pDef._range.min, maximum: pDef._range.max } : {}),
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
              const param = {
                name: paramName,
                in: loc,
                required: !!pDef.required,
                description: pDef.description || "",
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
