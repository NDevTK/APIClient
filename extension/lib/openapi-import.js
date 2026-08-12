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
  } else if (openapi.servers && openapi.servers.length > 0) {
    rootUrl = openapi.servers[0].url || "";
    // Interpolate server URL variables with their defaults
    const vars = openapi.servers[0].variables;
    if (vars) {
      for (const [vName, vDef] of Object.entries(vars)) {
        rootUrl = rootUrl.split(`{${vName}}`).join(vDef.default || vName);
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

  const doc = {
    kind: "discovery#restDescription",
    name:
      openapi.info?.title?.toLowerCase().replace(/[^a-z0-9]/g, "_") || "api",
    version: openapi.info?.version || "v1",
    title: openapi.info?.title || "Universal API",
    description: openapi.info?.description || "Converted from OpenAPI",
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
      // OAS 3.x: flows object; Swagger 2.0: single flow with scopes at top level
      const flows =
        scheme.flows ||
        (isSwagger2 && scheme.scopes
          ? { [scheme.flow || "implicit"]: { scopes: scheme.scopes } }
          : {});
      for (const [, flow] of Object.entries(flows)) {
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
  for (const [path, pathDef] of Object.entries(openapi.paths || {})) {
    const pathParams = pathDef.parameters || [];

    for (const [method, opDef] of Object.entries(pathDef)) {
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

      let methodName =
        opDef.operationId ||
        `${method.toLowerCase()}_${path.replace(/[^a-zA-Z0-9]/g, "_")}`;

      // Group by first tag or first path segment
      const tag =
        (opDef.tags && opDef.tags[0]) ||
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
      let realPath;
      if (opDef["x-original-path"]) {
        realPath = opDef["x-original-path"];
      } else {
        // Strip any `?x-operation=…` synthetic suffix from the path.
        realPath = path.replace(/[?&]x-operation=[^&]*/g, "").replace(/\?$/, "");
      }
      const m = {
        id: methodName,
        path: realPath.startsWith("/") ? realPath.substring(1) : realPath,
        httpMethod: method.toUpperCase(),
        description: opDef.description || opDef.summary || "",
        parameters: {},
        request: null,
        response: null,
      };
      doc.resources[resourceName].methods[methodName] = m;

      // Parameters: merge path-level + operation-level, skip body params (Swagger 2.0)
      const allParams = [...pathParams, ...(opDef.parameters || [])];
      for (const p of allParams) {
        if (p.in === "body") continue;
        m.parameters[p.name] = {
          type: p.schema?.type || p.type || "string",
          location: p.in || "query",
          required: !!p.required,
          description: p.description || "",
          enum: p.schema?.enum || p.enum || null,
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
        const bodyParam = allParams.find((p) => p.in === "body");
        if (bodyParam?.schema) reqSchema = bodyParam.schema;
      }

      // Form-urlencoded / multipart requestBody: restore properties as
      // body parameters so the learned discovery doc retains them in
      // m.parameters (which is what strict roundtrip + popup rendering
      // rely on). Without this, a form-POST method loses its field list
      // on IMPORT because the import only produces a schema ref.
      if (reqSchema && reqIsFormEncoded && !reqSchema.$ref && reqSchema.properties) {
        const requiredSet = new Set(reqSchema.required || []);
        const fieldNums = reqSchema["x-field-numbers"] || {};
        for (const [fName, fDef] of Object.entries(reqSchema.properties)) {
          if (m.parameters[fName]) continue; // don't clobber a named param
          m.parameters[fName] = {
            type: fDef.type || "string",
            location: "body",
            required: requiredSet.has(fName),
            description: fDef.description || "",
            enum: fDef.enum || null,
          };
          if (fieldNums[fName] != null) m.parameters[fName].number = fieldNums[fName];
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
