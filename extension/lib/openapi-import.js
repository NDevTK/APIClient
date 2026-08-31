/* OpenAPI/Swagger -> Discovery importer — extracted from discovery.js (classic script, globals; loaded in
   the worker AND popup). Converts a fetched OpenAPI/Swagger spec to the internal discovery format:
   resolveRef/flattenComposition/convertInlineSchema/convertSchemaProperty + convertOpenApiToDiscovery +
   extractMethodsFromDiscovery. The IMPORT pair to lib/openapi-export.js. */
// ─── OpenAPI/Swagger Conversion Helpers ───────────────────────────────────────

/**
 * Resolve a JSON $ref string to a local schema name.
 * Handles #/components/schemas/Foo, #/definitions/Foo, and similar local refs.
 */
function resolveRef(ref) {
  if (!ref || typeof ref !== "string") return ref;
  const hashIdx = ref.indexOf("#");
  if (hashIdx >= 0) {
    return ref.substring(hashIdx + 1).split("/").pop();
  }
  return ref.split("/").pop();
}

/**
 * Flatten allOf/oneOf/anyOf composition into a merged schema.
 * Recursively resolves $ref pointers within composition parts.
 * @param {object} schema - Schema that may contain composition keywords
 * @param {object} components - The components/definitions lookup
 * @returns {object} Flattened schema with merged properties
 */
function flattenComposition(schema, components) {
  if (!schema) return schema;
  const parts = schema.allOf || schema.oneOf || schema.anyOf;
  if (!parts || !Array.isArray(parts)) return schema;

  const merged = {
    type: schema.type || "object",
    properties: {},
    required: [...(schema.required || [])],
    description: schema.description || "",
  };
  if (schema.properties) {
    Object.assign(merged.properties, schema.properties);
  }

  // Iterative merge: walk parts BFS. When a part is itself a composition,
  // push ITS parts onto the queue instead of recursing. Cycle detection
  // by visited-set on resolved-schema identity so a composition that
  // references itself via $ref doesn't loop forever.
  const queue = [...parts];
  const visited = new Set();
  while (queue.length > 0) {
    const part = queue.shift();
    let resolved = part;
    if (part.$ref) {
      const name = resolveRef(part.$ref);
      resolved = components[name] || {};
    }
    if (visited.has(resolved)) continue;
    visited.add(resolved);
    const subParts = resolved.allOf || resolved.oneOf || resolved.anyOf;
    if (Array.isArray(subParts)) {
      // Pull resolved's own properties first, then queue its sub-parts
      // so they merge after — matches the recursive form's order.
      if (resolved.properties) Object.assign(merged.properties, resolved.properties);
      if (resolved.required) merged.required.push(...resolved.required);
      for (let si = 0; si < subParts.length; si++) queue.push(subParts[si]);
      continue;
    }
    if (resolved.properties) Object.assign(merged.properties, resolved.properties);
    if (resolved.required) merged.required.push(...resolved.required);
  }
  return merged;
}

/**
 * Convert an inline OpenAPI schema to our Discovery schema format.
 * Handles nested objects, arrays, composition, and enums.
 *
 * Iterative: nested schemas (composition results, array-item objects,
 * object-typed properties) used to recurse via convertInlineSchema /
 * convertSchemaProperty; that mutual recursion is now a worklist
 * drained before the entry-point function returns. The output shape
 * is unchanged — sub-schemas register under their generated name in
 * `docSchemas` and the parent property holds a `$ref` pointer.
 */
function convertInlineSchema(schema, name, components, docSchemas) {
  const deferred = [];
  const result = _convertInlineSchemaStep(schema, name, components, docSchemas, deferred);
  while (deferred.length > 0) {
    const d = deferred.shift();
    docSchemas[d.name] = _convertInlineSchemaStep(d.schema, d.name, components, docSchemas, deferred);
  }
  return result;
}

function _convertInlineSchemaStep(schema, name, components, docSchemas, deferred) {
  if (!schema) return { id: name, type: "object", properties: {} };
  const resolved = flattenComposition(schema, components);

  const result = {
    id: name,
    type: resolved.type || "object",
    required: resolved.required || [],
    properties: {},
  };

  if (resolved.properties) {
    for (const [pName, pDef] of Object.entries(resolved.properties)) {
      result.properties[pName] = _convertSchemaPropertyStep(
        pDef,
        name + "_" + pName.replace(/[^a-zA-Z0-9]/g, ""),
        components,
        docSchemas,
        deferred,
      );
    }
  }
  return result;
}

/**
 * Convert a single OpenAPI property definition to Discovery format.
 *
 * Iterative: nested schemas inside a property (composition payload,
 * array-item objects, object-typed sub-properties) are queued onto a
 * worklist that the entry-point drains. The property descriptor
 * returned here references those sub-schemas by their generated
 * `prefix`, which is also the key under which the sub-schema will be
 * registered in `docSchemas`.
 */
function convertSchemaProperty(pDef, prefix, components, docSchemas) {
  const deferred = [];
  const result = _convertSchemaPropertyStep(pDef, prefix, components, docSchemas, deferred);
  while (deferred.length > 0) {
    const d = deferred.shift();
    docSchemas[d.name] = _convertInlineSchemaStep(d.schema, d.name, components, docSchemas, deferred);
  }
  return result;
}

function _convertSchemaPropertyStep(pDef, prefix, components, docSchemas, deferred) {
  if (!pDef) return { type: "string", description: "" };

  if (pDef.$ref) {
    return {
      type: pDef.type || "string",
      description: pDef.description || "",
      $ref: resolveRef(pDef.$ref),
    };
  }

  // Composition in property — defer the flattened sub-schema onto the
  // worklist; the parent only needs the $ref pointer to that prefix,
  // which is known synchronously.
  if (pDef.allOf || pDef.oneOf || pDef.anyOf) {
    const flat = flattenComposition(pDef, components);
    if (flat.properties && Object.keys(flat.properties).length > 0) {
      deferred.push({ schema: flat, name: prefix });
      return {
        type: "object",
        $ref: prefix,
        description: pDef.description || "",
      };
    }
  }

  // Arrays
  if (pDef.type === "array" && pDef.items) {
    const items = {};
    if (pDef.items.$ref) {
      items.$ref = resolveRef(pDef.items.$ref);
    } else if (pDef.items.properties || pDef.items.allOf) {
      const itemName = prefix + "Item";
      deferred.push({ schema: pDef.items, name: itemName });
      items.$ref = itemName;
    } else {
      items.type = pDef.items.type || "string";
    }
    return { type: "array", items, description: pDef.description || "" };
  }

  // Nested inline objects
  if (pDef.type === "object" && pDef.properties) {
    deferred.push({ schema: pDef, name: prefix });
    return {
      type: "object",
      $ref: prefix,
      description: pDef.description || "",
    };
  }

  // Map types (additionalProperties)
  if (pDef.type === "object" && pDef.additionalProperties) {
    return {
      type: "string",
      description:
        (pDef.description || "") +
        " (map<string, " +
        (pDef.additionalProperties.type || "string") +
        ">)",
    };
  }

  // Scalar types
  const prop = {
    type: pDef.type || "string",
    format: pDef.format || null,
    description: pDef.description || "",
    $ref: null,
  };
  if (pDef.enum) {
    prop.enum = pDef.enum;
    prop.enumDescriptions = pDef["x-enumDescriptions"] || null;
  }
  return prop;
}

// ─── OpenAPI/Swagger → Discovery Converter ────────────────────────────────────

/**
 * Convert an OpenAPI 3.x or Swagger 2.0 document to our internal
 * Discovery-like format.
 *
 * Handles: Swagger 2.0 host/basePath/schemes, OAS 3.x servers with variables,
 * allOf/oneOf/anyOf composition, inline schemas, multiple content types,
 * multiple response codes, security schemes, and tag-based resource grouping.
 *
 * @param {object} openapi - Parsed OpenAPI/Swagger JSON
 * @param {string} sourceUrl - The URL it was fetched from
 * @returns {object} Normalized Discovery Doc
 */
function convertOpenApiToDiscovery(openapi, sourceUrl) {
  const u = new URL(sourceUrl);
  const isSwagger2 = !!openapi.swagger;
  const components = openapi.components?.schemas || openapi.definitions || {};

  // Resolve rootUrl: Swagger 2.0 uses host+basePath+schemes, OAS 3.x uses servers
  let rootUrl;
  if (isSwagger2 && openapi.host) {
    const rawScheme = (openapi.schemes && openapi.schemes[0]) || "https";
    const scheme = (rawScheme === "http" || rawScheme === "https") ? rawScheme : "https";
    const basePath = openapi.basePath || "/";
    // Validate host: must be a hostname with optional port, no protocol/path/special chars
    const host = openapi.host;
    if (!/^[a-zA-Z0-9._-]+(:\d+)?$/.test(host)) {
      rootUrl = `${u.origin}${basePath}`;
    } else {
      rootUrl = `${scheme}://${host}${basePath}`;
    }
  } else if (fdDocRecord(fdDocList(openapi.servers) === null ? null : openapi.servers[0]) !== null) {
    /* `servers: [null]` READ `.url` OFF NULL — a TypeError out of the trusted zone on a handed file. A
       non-record server entry describes no server, so the whole branch is refused and the `else` below
       resolves the root against the URL the spec was fetched from, which is what a document naming no
       server has said. */
    const server = openapi.servers[0];
    const serverUrl = fdDocString(server.url);
    rootUrl = serverUrl === null ? "" : serverUrl;
    // Interpolate server URL variables with their defaults
    const vars = fdDocRecord(server.variables);
    if (vars !== null) {
      for (const [vName, rawV] of Object.entries(vars)) {
        // `variables: {v: null}` names a variable and describes it not at all; its own name is then the
        // only thing the document said about it, which is what the existing fallback already substitutes.
        const vDef = fdDocRecord(rawV);
        const vDefault = vDef === null ? null : fdDocString(vDef.default);
        rootUrl = rootUrl.split(`{${vName}}`).join(vDefault === null ? vName : vDefault);
      }
    }
    // Resolve relative server URLs against source URL
    if (rootUrl && !/^https?:\/\//.test(rootUrl)) {
      try {
        rootUrl = new URL(rootUrl, sourceUrl).toString();
      } catch (_) {
        rootUrl = `${u.protocol}//${u.host}${rootUrl}`;
      }
    }
  } else {
    rootUrl = `${u.protocol}//${u.host}/`;
  }

  // Normalize: ensure trailing slash for consistent URL construction
  if (!rootUrl.endsWith("/")) rootUrl += "/";

  /* THE INFO BLOCK IS THE HANDED FILE'S TOO. `info: {title: 7}` called `.toLowerCase()` on a number and
     threw out of the trusted zone; refused, a document that titled itself with a non-string has titled
     itself with nothing, and the names below are what this converter calls a document that said none. */
  const info = fdDocRecord(openapi.info) === null ? {} : openapi.info;
  const infoTitle = fdDocString(info.title);
  const infoVersion = fdDocString(info.version);
  const infoDesc = fdDocString(info.description);
  const doc = {
    kind: "discovery#restDescription",
    name:
      (infoTitle === null ? "" : infoTitle.toLowerCase().replace(/[^a-z0-9]/g, "_")) || "api",
    version: infoVersion === null ? "v1" : infoVersion,
    title: infoTitle === null ? "Universal API" : infoTitle,
    description: infoDesc === null ? "Converted from OpenAPI" : infoDesc,
    rootUrl,
    servicePath: "",
    baseUrl: rootUrl,
    resources: {},
    schemas: {},
    auth: null,
  };

  // Convert schemas (components/definitions), flattening allOf/oneOf/anyOf
  for (const [name, schema] of Object.entries(components)) {
    const resolved = flattenComposition(schema, components);
    doc.schemas[name] = {
      id: name,
      type: resolved.type || "object",
      required: resolved.required || [],
      properties: {},
    };
    if (resolved.properties) {
      for (const [pName, pDef] of Object.entries(resolved.properties)) {
        doc.schemas[name].properties[pName] = convertSchemaProperty(
          pDef,
          name + "_" + pName.replace(/[^a-zA-Z0-9]/g, ""),
          components,
          doc.schemas,
        );
      }
    }
  }

  // Convert security schemes to auth metadata
  const securitySchemes =
    openapi.components?.securitySchemes || openapi.securityDefinitions || {};
  for (const [, scheme] of Object.entries(securitySchemes)) {
    if (scheme.type === "oauth2") {
      if (!doc.auth) doc.auth = { oauth2: { scopes: {} } };
      /* OAS 3.x states an oauth2 scheme's flows as a MAP KEYED BY FLOW NAME; Swagger 2.0 states one flow,
         with `flow` naming it and `scopes` beside it. Both are collected here as the LIST this loop actually
         consumes, because the flow NAME is read by nothing below — only `scopes` is. Keying a synthetic map
         by `scheme.flow || "implicit"` therefore invented an OAuth flow name for a document that named none,
         and invented the least restrictive of the four Swagger 2.0 flows at that, in a value no consumer
         ever looked at. A record with no reader is not a place to put a guess; it is a place to put nothing.
         `flows` off a third-party spec is refused rather than asserted (lib/field-def.js states that split):
         a `flows` that is not a record names no flows, which is what an empty list says. */
      const flowRecord = fdDocRecord(scheme.flows);
      const flowList = flowRecord !== null
        ? Object.values(flowRecord)
        : (isSwagger2 && scheme.scopes ? [{ scopes: scheme.scopes }] : []);
      for (const flow of flowList) {
        if (flow?.scopes) {
          for (const [scope, desc] of Object.entries(flow.scopes)) {
            doc.auth.oauth2.scopes[scope] = {
              description: typeof desc === "string" ? desc : "",
            };
          }
        }
      }
    } else if (scheme.type === "apiKey") {
      if (!doc.auth) doc.auth = {};
      doc.auth.apiKey = { name: scheme.name, in: scheme.in };
    }
  }

  // Convert paths to methods, grouped by tag or path prefix
  /* `paths` AND EACH PATH ITEM ARE THE HANDED FILE'S. `paths: {"/p": null}` read `.parameters` off null and
     threw out of the trusted zone; a non-record path item describes no operations, so it is skipped, which
     is what that document states about that path. */
  const paths = fdDocRecord(openapi.paths) === null ? {} : openapi.paths;
  for (const [path, rawPathDef] of Object.entries(paths)) {
    const pathDef = fdDocRecord(rawPathDef);
    if (pathDef === null) continue;
    const pathParams = pathDef.parameters;

    for (const [method, rawOpDef] of Object.entries(pathDef)) {
      // OpenAPI's spec-defined verbs: get/put/post/delete/options/head/patch/trace.
      // The previous restriction to 5 silently dropped HEAD/OPTIONS/TRACE
      // methods on import, causing method count drift on roundtrip.
      if (
        !["get", "post", "put", "delete", "patch", "head", "options", "trace"].includes(
          method.toLowerCase(),
        )
      ) {
        continue;
      }

      /* `get: null` IS A VERB NAMING NO OPERATION — refused rather than read through. */
      const opDef = fdDocRecord(rawOpDef);
      if (opDef === null) continue;

      /* THE METHOD'S NAME AND ITS RESOURCE ARE BOTH THE HANDED FILE'S TEXT, AND BOTH WERE TAKEN RAW.
         `operationId: {}` became the method literally KEYED "[object Object]" — a name no document wrote,
         stringified into the discovery doc where it reads exactly like one that did. `tags: [7]` and
         `tags: [{}]` called `.toLowerCase()` on the value and threw out of the trusted zone instead.
         Refused, both fall to the answer that was always beneath them: the path's own first segment names
         the resource, and verb_path names the operation, which is what this converter calls an operation a
         document did not name. */
      const opId = fdDocString(opDef.operationId);
      let methodName =
        (opId === null ? "" : opId) ||
        `${method.toLowerCase()}_${path.replace(/[^a-zA-Z0-9]/g, "_")}`;

      // Group by first tag or first path segment
      const tagList = fdDocList(opDef.tags);
      const firstTag = tagList === null || tagList.length === 0 ? null : fdDocString(tagList[0]);
      const tag =
        (firstTag === null ? "" : firstTag) ||
        path.split("/").filter(Boolean)[0] ||
        "default";
      const resourceName = tag.toLowerCase().replace(/[^a-z0-9_]/g, "_");
      if (!doc.resources[resourceName]) {
        doc.resources[resourceName] = { methods: {} };
      }
      // Two ops can share an operationId but differ by HTTP verb (e.g. a
      // probed POST and learned GET against the same /path). Qualify with
      // the verb when a name collision would otherwise drop an entry.
      if (doc.resources[resourceName].methods[methodName]) {
        methodName = method.toLowerCase() + "_" + methodName;
      }

      // If the export added an x-operation discriminator to disambiguate
      // multiple ops on the same (path, verb), prefer x-original-path so
      // the resulting discovery doc keeps the real URL path.
      /* `x-original-path: 7` REACHED `.startsWith` AND THREW. It is our OWN export's extension key, but it
         arrives on a file anyone may have edited, so it is refused like every other read here — and a
         non-string one names no path, which leaves the real path the one this entry is keyed by. */
      const origPath = fdDocString(opDef["x-original-path"]);
      const realPath = origPath !== null && origPath !== ""
        ? origPath
        // Strip any `?x-operation=…` synthetic suffix from the path.
        : path.replace(/[?&]x-operation=[^&]*/g, "").replace(/\?$/, "");
      const opDesc = fdDocString(opDef.description);
      const opSummary = fdDocString(opDef.summary);
      const m = {
        id: methodName,
        path: realPath.startsWith("/") ? realPath.substring(1) : realPath,
        httpMethod: method.toUpperCase(),
        description: (opDesc === null ? "" : opDesc) || (opSummary === null ? "" : opSummary) || "",
        parameters: {},
        request: null,
        response: null,
      };
      doc.resources[resourceName].methods[methodName] = m;

      // Parameters: merge path-level + operation-level, skip body params (Swagger 2.0)
      /* EVERY READ BELOW IS A REFUSAL, because this is a spec file the researcher was handed and the trusted
         zone must not abort on it — nor fabricate out of it. Four of these were TypeErrors thrown straight
         out of this loop (`parameters: [null]` read `.in` off null) and three were silent inventions that
         reached the Send panel indistinguishable from declared facts: a parameter with no `name` was keyed
         `"undefined"`, one whose `name` was an object was keyed `"[object Object]"`, and one with no `in` was
         placed in the QUERY STRING — which is §@S's wrong report exactly, a fabricated placement rendered
         identically to a stated one, and it decided where a value would actually be SENT. */
      const allParams = [...(fdDocList(pathParams) === null ? [] : pathParams),
                         ...(fdDocList(opDef.parameters) === null ? [] : opDef.parameters)];
      for (const rawP of allParams) {
        /* A NON-RECORD IS NOT A PARAMETER OBJECT. Refusing it whole is the true statement about it; the
           alternative read `.in` off whatever it was. */
        const p = fdDocRecord(rawP);
        if (p === null) continue;
        if (p.in === "body") continue;
        /* `name` AND `in` ARE BOTH **REQUIRED** — OpenAPI Specification 3.1.1 §Parameter Object states it of
           each, and Swagger 2.0 §Parameter Object likewise. An object missing either is not a parameter, so
           there is nothing here to render and nothing to invent: it is SKIPPED, which is what the document
           said. `fdDocLocation` also refuses an `in` neither spec defines. */
        const pName = fdDocString(p.name);
        if (pName === null) continue;
        const pLoc = fdDocLocation(p.in);
        if (pLoc === null) continue;
        const pSchema = fdDocRecord(p.schema) === null ? {} : p.schema;
        /* "string" IS THE TYPE OF A PROPERTY WHOSE DOCUMENT SAYS NOTHING ELSE — the same answer
           lib/discovery.js's `mapJsonSchemaType` gives for exactly that case, so it is a stated fact and not
           a hole. `enum`/`description` have declared absences and take them. */
        const pType = fdDocString(pSchema.type) === null ? fdDocString(p.type) : fdDocString(pSchema.type);
        const pEnum = fdDocList(pSchema.enum) === null ? fdDocList(p.enum) : fdDocList(pSchema.enum);
        const pDesc = fdDocString(p.description);
        m.parameters[pName] = {
          type: pType === null ? "string" : pType,
          location: pLoc,
          required: p.required === true,
          description: pDesc === null ? "" : pDesc,
          enum: pEnum,
        };
      }

      // Request body
      let reqSchema = null;
      let reqIsFormEncoded = false;
      if (!isSwagger2 && opDef.requestBody?.content) {
        // OAS 3.x: try JSON first, then form types, then any available
        const content = opDef.requestBody.content;
        const preferred = [
          "application/json",
          "application/x-www-form-urlencoded",
          "multipart/form-data",
        ];
        for (const ct of preferred) {
          if (content[ct]?.schema) {
            reqSchema = content[ct].schema;
            reqIsFormEncoded = ct !== "application/json";
            break;
          }
        }
        if (!reqSchema) {
          const firstKey = Object.keys(content)[0];
          if (firstKey) {
            reqSchema = content[firstKey]?.schema;
            reqIsFormEncoded = firstKey !== "application/json";
          }
        }
      } else if (isSwagger2) {
        // Swagger 2.0: body parameter
        // Same refusal as the loop above — `parameters: [null]` reaches here too, and `.in` off null is the
        // identical TypeError one branch over.
        const bodyParam = allParams.find((p) => fdDocRecord(p) !== null && p.in === "body");
        if (bodyParam?.schema) reqSchema = bodyParam.schema;
      }

      // Form-urlencoded / multipart requestBody: restore properties as
      // body parameters so the learned discovery doc retains them in
      // m.parameters (which is what strict roundtrip + popup rendering
      // rely on). Without this, a form-POST method loses its field list
      // on IMPORT because the import only produces a schema ref.
      /* THE SAME REFUSALS, AND THE SAME TWO FAILURES, one line apart. `properties: "ab"` is not a property
         map, and `Object.entries` on a STRING yields index keys — so that document minted two body
         parameters named "0" and "1" that it had never declared. `required: "ab"` is not a required list, and
         `new Set("ab")` is a set of CHARACTERS — so a field named `a` came back REQUIRED because the string
         happened to contain the letter, a fabricated cardinality claim the panel renders as a badge. Both
         are refused whole: a document that stated no property map declares no body fields. */
      const reqProps = reqSchema && reqIsFormEncoded && !reqSchema.$ref ? fdDocRecord(reqSchema.properties) : null;
      if (reqProps !== null) {
        const reqRequired = fdDocList(reqSchema.required);
        const requiredSet = new Set(reqRequired === null ? [] : reqRequired);
        const fieldNums = fdDocRecord(reqSchema["x-field-numbers"]) === null ? {} : reqSchema["x-field-numbers"];
        for (const [fName, rawF] of Object.entries(reqProps)) {
          if (m.parameters[fName]) continue; // don't clobber a named param
          /* `{"a": null}` NAMED A PROPERTY AND DESCRIBED NOTHING — the key is still a field the document
             declared, so it is kept and every value it did not state takes its absence. Reading `.type`
             straight off it was a TypeError out of the trusted zone, the same one lib/discovery.js's
             property walker was already fixed for. */
          const fDef = fdDocRecord(rawF) === null ? {} : rawF;
          const fType = fdDocString(fDef.type);
          const fDesc = fdDocString(fDef.description);
          m.parameters[fName] = {
            type: fType === null ? "string" : fType,
            location: "body",
            required: requiredSet.has(fName),
            description: fDesc === null ? "" : fDesc,
            enum: fdDocList(fDef.enum),
          };
          /* THE IMPORTED FIELD NUMBER IS REFUSED, NOT TAKEN. This is a file the researcher was handed, and
             the number reaches the Send panel's own record (lib/field-def.js), which asserts a scalar-or-
             null because by then every value came through a refusal like this one. `x-field-numbers: {"a":
             {}}` names no field number under any reading, and null is the true statement about it — the
             alternative is the trusted zone aborting on bytes somebody else chose. */
          const importedNum = fdDocKey(fieldNums[fName]);
          if (importedNum !== null) m.parameters[fName].number = importedNum;
        }
        // Restored inline — don't also create a synthetic request schema.
        reqSchema = null;
      }

      if (reqSchema) {
        if (reqSchema.$ref) {
          m.request = { $ref: resolveRef(reqSchema.$ref) };
        } else if (
          reqSchema.properties ||
          reqSchema.allOf ||
          reqSchema.oneOf ||
          reqSchema.anyOf ||
          reqSchema.type === "object"
        ) {
          const synName = methodName + "Request";
          doc.schemas[synName] = convertInlineSchema(
            reqSchema,
            synName,
            components,
            doc.schemas,
          );
          m.request = { $ref: synName };
        }
      }

      // Response body: try multiple success codes + default
      let respSchema = null;
      if (opDef.responses) {
        const successCodes = ["200", "201", "202", "203", "204", "default"];
        for (const code of successCodes) {
          const resp = opDef.responses[code];
          if (!resp) continue;
          // OAS 3.x
          if (resp.content) {
            let rSchema = resp.content["application/json"]?.schema;
            if (!rSchema) {
              const rKey = Object.keys(resp.content)[0];
              if (rKey) rSchema = resp.content[rKey]?.schema;
            }
            if (rSchema) {
              respSchema = rSchema;
              break;
            }
          }
          // Swagger 2.0
          if (resp.schema) {
            respSchema = resp.schema;
            break;
          }
        }
      }

      if (respSchema) {
        if (respSchema.$ref) {
          m.response = { $ref: resolveRef(respSchema.$ref) };
        } else if (
          respSchema.properties ||
          respSchema.allOf ||
          respSchema.oneOf ||
          respSchema.anyOf ||
          respSchema.type === "object"
        ) {
          const synName = methodName + "Response";
          doc.schemas[synName] = convertInlineSchema(
            respSchema,
            synName,
            components,
            doc.schemas,
          );
          m.response = { $ref: synName };
        }
      }
    }
  }

  return doc;
}

/**
 * Extract all methods from a discovery document as flat endpoint list.
 * @param {object} doc - Parsed discovery JSON
 * @returns {Array<{id, httpMethod, fullPath, scopes, parameters}>}
 */
function extractMethodsFromDiscovery(doc) {
  if (!doc) return [];

  const methods = [];
  const baseUrl = doc.baseUrl || doc.rootUrl || "";

  function walk(res) {
    for (const [, r] of Object.entries(res || {})) {
      for (const [, m] of Object.entries(r.methods || {})) {
        methods.push({
          id: m.id,
          httpMethod: m.httpMethod,
          fullPath: baseUrl + (m.path || m.flatPath || ""),
          scopes: m.scopes || [],
          parameters: m.parameters ? Object.keys(m.parameters) : [],
          description: m.description,
        });
      }
      if (r.resources) walk(r.resources);
    }
  }

  walk(doc.resources);
  return methods;
}
