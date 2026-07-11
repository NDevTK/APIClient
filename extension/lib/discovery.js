// Discovery document fetcher and parser.
// Tries multiple URL patterns and auth strategies to locate
// the REST discovery document for a given Google API service.
//
// Strategies from the research:
//  - Plain GET (public APIs)
//  - ?labels=PANTHEON (visibility label expansion, "Decoding Google" article)
//  - With API key in URL param or X-Goog-Api-Key header
//  - POST + X-Http-Method-Override: GET (bypasses 405 on some services like youtubei)
//  - Staging sandbox variant (staging-<svc>.sandbox.googleapis.com)
//  - clients6.google.com variant

/**
 * Build candidate discovery URLs for a given hostname.
 * @param {string} hostname - e.g. "people-pa.googleapis.com"
 * @param {string|null} apiKey
 * @returns {Array<{url: string, headers: object, method: string}>}
 */
function buildDiscoveryUrls(hostname, apiKey) {
  const candidates = [];

  // 1. Generic Universal Patterns (OpenAPI / Swagger)
  // These work on almost any modern API domain
  const genericPaths = [
    "/.well-known/openapi.json",
    "/.well-known/swagger.json",
    "/openapi.json",
    "/swagger.json",
    "/swagger/v1/swagger.json",
    "/api/docs",
    "/api/v1/docs",
    "/api-docs",
    "/v1/api-docs",
  ];

  for (const path of genericPaths) {
    candidates.push({
      url: `https://${hostname}${path}#_internal_probe`,
      headers: {},
      method: "GET",
    });
  }

  // 2. Google-Specific Patterns
  // Normalize: if it's a clients6 host, also try the googleapis.com equivalent
  const hosts = [hostname];
  const clients6Suffix = ".clients6.google.com";
  const googleapisSuffix = ".googleapis.com";
  const isClients6Host =
    hostname === clients6Suffix ||
    hostname.endsWith(clients6Suffix);
  const isGoogleapisHost =
    hostname === googleapisSuffix ||
    hostname.endsWith(googleapisSuffix);
  if (isClients6Host) {
    hosts.push(hostname.replace(clients6Suffix, googleapisSuffix));
  } else if (isGoogleapisHost && !hostname.includes("sandbox")) {
    hosts.push(hostname.replace(googleapisSuffix, clients6Suffix));
  }

  // Common version strings to try — some services require explicit ?version=
  const versions = ["v1", "v2", "v1beta1", "v1alpha1"];

  for (const host of hosts) {
    const base = `https://${host}/$discovery/rest`;

    // Plain GET
    candidates.push({
      url: `${base}#_internal_probe`,
      headers: {},
      method: "GET",
    });

    // Visibility label expansion
    candidates.push({
      url: `${base}?labels=PANTHEON#_internal_probe`,
      headers: {},
      method: "GET",
    });

    // Versions
    for (const ver of versions) {
      candidates.push({
        url: `${base}?version=${ver}#_internal_probe`,
        headers: {},
        method: "GET",
      });
    }

    // With API key
    if (apiKey) {
      candidates.push({
        url: `${base}?key=${apiKey}#_internal_probe`,
        headers: {},
        method: "GET",
      });
      candidates.push({
        url: `${base}#_internal_probe`,
        headers: { "X-Goog-Api-Key": apiKey },
        method: "GET",
      });
    }

    // POST override
    candidates.push({
      url: `${base}#_internal_probe`,
      headers: { "X-Http-Method-Override": "GET" },
      method: "POST",
    });
  }

  return candidates;
}

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

// ─── Schema Resolution (for Send Request form) ─────────────────────────────

/**
 * Find a discovery method matching an endpoint's path and HTTP method.
 * Walks doc.resources recursively, comparing method paths.
 *
 * @param {object} doc - Full parsed discovery JSON
 * @param {string} endpointPath - URL path (e.g. "/v1/people:search")
 * @param {string} httpMethod - HTTP method (e.g. "POST")
 * @returns {{method: object, resourceName: string}|null}
 */
function findDiscoveryMethod(doc, endpointPath, httpMethod) {
  if (!doc || !doc.resources) return null;

  // canParse guard so the basePath extraction doesn't use throw as a
  // parse-validity test. An empty / malformed baseUrl yields empty basePath,
  // which means no prefix stripping below — correct semantic.
  const baseUrl = doc.baseUrl || doc.rootUrl || "";
  const basePath = (baseUrl && URL.canParse(baseUrl))
    ? new URL(baseUrl).pathname.replace(/\/$/, "")
    : "";

  // Strip basePath prefix from endpointPath for comparison
  let normPath = endpointPath;
  if (basePath && normPath.startsWith(basePath)) {
    normPath = normPath.slice(basePath.length);
  }
  normPath = normPath.replace(/^\//, "");

  function normalizePath(p) {
    // Convert {param} placeholders to a wildcard for matching
    return (p || "").replace(/^\//, "").replace(/\{[^}]+\}/g, "*");
  }

  function matchPath(methodPath, target) {
    const a = normalizePath(methodPath);
    const b = target.replace(/\{[^}]+\}/g, "*");
    if (a === b) return true;
    // Also try matching with path params as segments
    const aParts = a.split("/");
    const bParts = b.split("/");
    if (aParts.length !== bParts.length) return false;
    for (let i = 0; i < aParts.length; i++) {
      if (aParts[i] === "*" || bParts[i] === "*") continue;
      if (aParts[i] !== bParts[i]) return false;
    }
    return true;
  }

  let best = null;

  function walk(res, prefix) {
    for (const [name, r] of Object.entries(res || {})) {
      const fullName = prefix ? prefix + "." + name : name;
      for (const [, m] of Object.entries(r.methods || {})) {
        const mMethod = (m.httpMethod || "").toUpperCase();
        const mPath = m.flatPath || m.path || "";
        if (
          mMethod === httpMethod.toUpperCase() &&
          matchPath(mPath, normPath)
        ) {
          best = { method: m, resourceName: fullName };
          return;
        }
      }
      if (r.resources) walk(r.resources, fullName);
      if (best) return;
    }
  }

  walk(doc.resources, "");

  // Fallback: partial match (endsWith) for flexibility
  if (!best) {
    function walkPartial(res, prefix) {
      for (const [name, r] of Object.entries(res || {})) {
        const fullName = prefix ? prefix + "." + name : name;
        for (const [, m] of Object.entries(r.methods || {})) {
          const mPath = normalizePath(m.flatPath || m.path || "");
          if (
            normPath.endsWith(mPath) ||
            mPath.endsWith(normPath.replace(/\{[^}]+\}/g, "*"))
          ) {
            best = { method: m, resourceName: fullName };
            return;
          }
        }
        if (r.resources) walkPartial(r.resources, fullName);
        if (best) return;
      }
    }
    walkPartial(doc.resources, "");
  }

  return best;
}

/**
 * Find a discovery method by its ID (e.g. "people.people.get").
 *
 * @param {object} doc - Full parsed discovery JSON
 * @param {string} methodId - The method ID to find
 * @returns {{method: object, resourceName: string}|null}
 */
function findMethodById(doc, methodId) {
  if (!doc || !doc.resources) return null;

  let best = null;

  function walk(res, prefix) {
    for (const [name, r] of Object.entries(res || {})) {
      const fullName = prefix ? prefix + "." + name : name;
      for (const [, m] of Object.entries(r.methods || {})) {
        if (m.id === methodId) {
          best = { method: m, resourceName: fullName };
          return;
        }
      }
      if (r.resources) walk(r.resources, fullName);
      if (best) return;
    }
  }

  walk(doc.resources, "");
  return best;
}

/**
 * Resolve a discovery document schema into a recursive field list.
 * Follows $ref pointers in doc.schemas to build the full type tree.
 *
 * Iterative driver — single shared queue processes SCHEMA frames
 * (resolve a named schema's fields into a target array) and PROP
 * frames (populate a single field from a prop definition). Cycles
 * are detected by a per-chain visited set carried on each frame:
 * the set reflects the chain from the root through nested $refs to
 * the current frame, so a back-reference adds a self-circular
 * sentinel field to the children array. No depth cap — visited set
 * is structurally sufficient for any finite OpenAPI schema graph.
 *
 * @param {object} doc - Full parsed discovery JSON
 * @param {string} schemaName - Schema name to resolve (e.g. "Person")
 * @returns {Array<{name, type, required, description, label, children}>}
 */
function resolveDiscoverySchema(doc, schemaName) {
  var fields = [];
  if (!doc || !doc.schemas || !doc.schemas[schemaName]) return fields;
  fields.id = schemaName;
  var queue = [{ kind: "SCHEMA", doc: doc, schemaName: schemaName,
                  visited: new Set(), into: fields }];
  _drainDiscoveryQueue(queue);
  return fields;
}

/**
 * Map a single discovery document property to a unified field descriptor.
 *
 * Iterative driver — same queue and step functions as
 * resolveDiscoverySchema. The entry-point pre-allocates the root
 * field shell, seeds the queue with a PROP frame targeting it, and
 * drains the queue before returning.
 */
function mapDiscoveryProperty(doc, name, prop, requiredList) {
  var root = _buildDiscoveryFieldShell(name, prop, requiredList);
  var queue = [{ kind: "PROP", doc: doc, field: root, prop: prop,
                  visited: new Set() }];
  _drainDiscoveryQueue(queue);
  return root;
}

function _buildDiscoveryFieldShell(name, prop, requiredList) {
  var isRequired = (requiredList || []).indexOf(name) >= 0;
  return {
    name: prop.name || name,
    customName: !!prop.customName,
    type: "string",
    required: isRequired,
    description: prop.description || null,
    label: isRequired ? "required" : "optional",
    number: prop.id != null ? prop.id : null,
    messageType: null,
    children: null,
    _defaultValue: prop._defaultValue == null ? null : prop._defaultValue,
    _defaultConfidence: prop._defaultConfidence == null ? null : prop._defaultConfidence,
    _requiredConfidence: prop._requiredConfidence == null ? null : prop._requiredConfidence,
    _range: prop._range || null,
    _detectedEnum: !!prop._detectedEnum,
    _exampleValue: prop._exampleValue === undefined ? null : prop._exampleValue,
    _exampleValueSource: prop._exampleValueSource || null,
    _astValidValues: prop._astValidValues || null,
  };
}

// Sentinel field appended to a children array when a $ref points back
// to a schema already on the current chain. The OpenAPI/JSON-Schema
// spec permits self-referential schemas, so a finite tree
// representation must terminate the cycle somewhere — the sentinel
// makes the truncation point visible to callers (form builders,
// renderers) instead of silently dropping data.
function _circularRefSentinel(schemaName) {
  return {
    name: "...",
    type: "message",
    description: "(circular ref: " + schemaName + ")",
    label: "optional",
  };
}

function _drainDiscoveryQueue(queue) {
  // LIFO drain: the only observable side effect is registration into
  // docSchemas keyed by schema name, so order doesn't matter and pop()
  // avoids the O(N) cost of shift().
  while (queue.length > 0) {
    var item = queue.pop();
    if (item.kind === "SCHEMA") _stepResolveSchema(item, queue);
    else if (item.kind === "PROP") _stepMapProperty(item, queue);
  }
}

// SCHEMA frame: produce field shells for each property of
// `doc.schemas[schemaName]`, append them to `into`, and queue PROP
// frames that populate each shell. Cycle check: if `schemaName` is
// already on the current chain (visited set), append the circular-ref
// sentinel and skip processing.
function _stepResolveSchema(item, queue) {
  var doc = item.doc, schemaName = item.schemaName, visited = item.visited, into = item.into;
  if (!doc || !doc.schemas || !doc.schemas[schemaName]) return;
  if (visited.has(schemaName)) {
    into.push(_circularRefSentinel(schemaName));
    return;
  }
  var nextVisited = new Set(visited);
  nextVisited.add(schemaName);
  var schema = doc.schemas[schemaName];
  var required = schema.required || [];
  var i = 1;
  for (var propName in schema.properties || {}) {
    var prop = schema.properties[propName];
    var shell = _buildDiscoveryFieldShell(propName, prop, required);
    if (shell.number == null) {
      shell.number = i;
      shell.isNumberGuessed = true;
    }
    into.push(shell);
    queue.push({ kind: "PROP", doc: doc, field: shell, prop: prop, visited: nextVisited });
    i++;
  }
}

// PROP frame: populate the pre-allocated `field` from `prop`. $ref →
// queue a SCHEMA frame whose `into` is the field's children array.
// Inline object / array-of-object → queue child PROP frames. The
// visited set carried into sub-frames is the chain from root to the
// current node; when a sub-frame recurses through a $ref, it gets a
// fresh clone via _stepResolveSchema's `nextVisited`.
function _stepMapProperty(item, queue) {
  var doc = item.doc, f = item.field, p = item.prop, v = item.visited;

  if (p.$ref) {
    f.type = "message";
    f.messageType = p.$ref;
    f.children = [];
    queue.push({ kind: "SCHEMA", doc: doc, schemaName: p.$ref,
                  visited: v, into: f.children });
    return;
  }

  if (p.type === "array" && p.items) {
    f.label = "repeated";
    if (p.items.$ref) {
      f.type = "message";
      f.messageType = p.items.$ref;
      f.children = [];
      queue.push({ kind: "SCHEMA", doc: doc, schemaName: p.items.$ref,
                    visited: v, into: f.children });
    } else if (p.items.type === "object" && p.items.properties) {
      f.type = "message";
      f.children = [];
      var arrRequired = p.items.required || [];
      for (var ipn in p.items.properties) {
        var arrShell = _buildDiscoveryFieldShell(ipn, p.items.properties[ipn], arrRequired);
        f.children.push(arrShell);
        queue.push({ kind: "PROP", doc: doc, field: arrShell,
                      prop: p.items.properties[ipn], visited: v });
      }
    } else {
      f.type = mapJsonSchemaType(p.items);
    }
    return;
  }

  if (p.type === "object" && p.properties) {
    f.type = "message";
    f.children = [];
    var nestedRequired = p.required || [];
    for (var pn in p.properties) {
      var nestShell = _buildDiscoveryFieldShell(pn, p.properties[pn], nestedRequired);
      f.children.push(nestShell);
      queue.push({ kind: "PROP", doc: doc, field: nestShell,
                    prop: p.properties[pn], visited: v });
    }
    return;
  }

  if (p.type === "object" && p.additionalProperties) {
    f.type = "string";
    f.description =
      (f.description || "") +
      " (map<string, " +
      (p.additionalProperties.type || "string") +
      ">)";
    return;
  }

  // Scalar
  f.type = mapJsonSchemaType(p);
  if (p.label === "repeated") f.label = "repeated";
  if (p.enum) {
    f.type = "enum";
    f.enum = p.enum;
    f.enumValues = p.enum;
    f.enumDescriptions = p.enumDescriptions || null;
  }
}

/**
 * Map a JSON schema type+format to unified protobuf-style type.
 */
function mapJsonSchemaType(prop) {
  if (!prop) return "string";
  var t = prop.type || "string";
  var f = prop.format || "";

  // Pass through protobuf-native types (from JSPB-learned schemas)
  var pbTypes = [
    "int32", "int64", "uint32", "uint64", "sint32", "sint64",
    "double", "float", "fixed32", "fixed64", "sfixed32", "sfixed64",
    "bool", "bytes", "enum",
  ];
  if (pbTypes.indexOf(t) >= 0) return t;

  if (t === "string") {
    if (f === "byte") return "bytes";
    if (f === "int64" || f === "uint64") return f;
    return "string";
  }
  if (t === "integer") {
    if (f === "int32" || f === "uint32") return f;
    return "int32";
  }
  if (t === "number") {
    if (f === "float") return "float";
    return "double";
  }
  if (t === "boolean") return "bool";
  return "string";
}

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
              const loc = pDef.location || "query";
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

// ─── Content-based asset classification ──────────────────────────────────────
//
// Decide whether a captured response body is binary media (image, video,
// font, archive, 3d model, wasm) purely from magic bytes — no URL extension,
// no content-type. An API endpoint that returns a PNG is still an API
// (its URL/query/auth are meaningful); this classifier only decides whether
// to attempt structured-schema extraction from the response body. Binary
// media has no JSON/protobuf schema to learn, so we skip response parsing
// and annotate the method entry with the detected media type.
//
// Nothing is hidden from the user — every captured response appears in the
// log with its _assetKind / _assetLabel so a signed-URL photo endpoint,
// avatar API, or CDN asset is all visible. The classifier just prevents the
// extension from synthesizing a response schema from random bytes.
//
// Sniff magic bytes on a Uint8Array. Returns a MIME-like label or null.
function sniffBinaryMagic(bytes) {
  if (!bytes || bytes.length < 2) return null;
  var b = bytes;
  if (b.length >= 8 && b[0] === 0x89 && b[1] === 0x50 && b[2] === 0x4e && b[3] === 0x47) return "image/png";
  if (b.length >= 3 && b[0] === 0xff && b[1] === 0xd8 && b[2] === 0xff) return "image/jpeg";
  if (b.length >= 6 && b[0] === 0x47 && b[1] === 0x49 && b[2] === 0x46 && b[3] === 0x38) return "image/gif";
  if (b.length >= 12 && b[0] === 0x52 && b[1] === 0x49 && b[2] === 0x46 && b[3] === 0x46 && b[8] === 0x57 && b[9] === 0x45 && b[10] === 0x42 && b[11] === 0x50) return "image/webp";
  if (b.length >= 4 && b[0] === 0x25 && b[1] === 0x50 && b[2] === 0x44 && b[3] === 0x46) return "application/pdf";
  if (b.length >= 4 && b[0] === 0x77 && b[1] === 0x4f && b[2] === 0x46 && b[3] === 0x46) return "font/woff";
  if (b.length >= 4 && b[0] === 0x77 && b[1] === 0x4f && b[2] === 0x46 && b[3] === 0x32) return "font/woff2";
  if (b.length >= 4 && b[0] === 0x00 && b[1] === 0x01 && b[2] === 0x00 && b[3] === 0x00) return "font/ttf";
  if (b.length >= 4 && b[0] === 0x4f && b[1] === 0x54 && b[2] === 0x54 && b[3] === 0x4f) return "font/otf";
  if (b.length >= 2 && b[0] === 0x1f && b[1] === 0x8b) return "application/gzip";
  if (b.length >= 4 && b[0] === 0x50 && b[1] === 0x4b && b[2] === 0x03 && b[3] === 0x04) return "application/zip";
  if (b.length >= 4 && b[0] === 0x25 && b[1] === 0x21 && b[2] === 0x50 && b[3] === 0x53) return "application/postscript";
  if (b.length >= 4 && b[0] === 0x00 && b[1] === 0x61 && b[2] === 0x73 && b[3] === 0x6d) return "application/wasm";
  // MP4 / QuickTime: bytes 4..7 are "ftyp"
  if (b.length >= 8 && b[4] === 0x66 && b[5] === 0x74 && b[6] === 0x79 && b[7] === 0x70) return "video/mp4";
  if (b.length >= 4 && b[0] === 0x1a && b[1] === 0x45 && b[2] === 0xdf && b[3] === 0xa3) return "video/webm";
  // glTF (.glb) — little-endian "glTF" magic
  if (b.length >= 4 && b[0] === 0x67 && b[1] === 0x6c && b[2] === 0x54 && b[3] === 0x46) return "model/gltf-binary";
  // RIFF container (wav/avi) — webp already matched above
  if (b.length >= 4 && b[0] === 0x52 && b[1] === 0x49 && b[2] === 0x46 && b[3] === 0x46) return "application/octet-stream";
  // ID3/MP3
  if (b.length >= 3 && b[0] === 0x49 && b[1] === 0x44 && b[2] === 0x33) return "audio/mpeg";
  // OGG
  if (b.length >= 4 && b[0] === 0x4f && b[1] === 0x67 && b[2] === 0x67 && b[3] === 0x53) return "audio/ogg";
  return null;
}

// Classify the response body. Returns { kind, label }:
//   "asset"   → binary media; skip RESPONSE-body schema extraction (request
//                still learned as normal). label is the sniffed MIME.
//   "empty"   → no body captured; learn as a fire-and-forget API (204-style).
//   "api"     → structured or text body; learn normally.
// Text-format asset signatures (SVG, plain CSS from CDN, etc.). SVG in
// particular is text but structurally a static image — icon CDNs like
// fonts.gstatic.com serve thousands of per-icon GET responses that
// shouldn't populate the discovery doc. Sniff on leading bytes only.
function _sniffTextAssetSignature(text) {
  if (!text) return null;
  var head = text.trimStart();
  var t = head.slice(0, 512);
  var lower = t.toLowerCase();
  // HLS playlist — literal "#EXTM3U" on the first line.
  if (head.startsWith("#EXTM3U")) return "application/vnd.apple.mpegurl";
  // WebVTT subtitles — "WEBVTT" header line.
  if (head.startsWith("WEBVTT")) return "text/vtt";
  // SVG (two entry shapes)
  if (lower.startsWith("<?xml") && /<svg\b/.test(lower)) return "image/svg+xml";
  if (lower.startsWith("<svg")) return "image/svg+xml";
  // HTML — doctype declaration or root <html> tag. Page fragments and
  // full documents fetched via fetch() are assets, not APIs.
  if (lower.startsWith("<!doctype html")) return "text/html";
  if (lower.startsWith("<html")) return "text/html";
  // DASH manifest — XML with <MPD as root element.
  if (lower.startsWith("<?xml") && /<mpd\b/.test(lower)) return "application/dash+xml";
  // SMIL / SRT — some streamers use these.
  if (lower.startsWith("<?xml") && /<smil\b/.test(lower)) return "application/smil+xml";
  // Plain CSS (CDN icon fonts often ship CSS with @font-face rules).
  // Require a @-rule at the head to avoid matching HTML with inline <style>.
  if (/^@(font-face|import|charset|media|keyframes|supports)\b/.test(t)) return "text/css";
  return null;
}

function classifyResponseAsset(responseBody, responseBase64, opts) {
  // Opaque cross-origin responses (fetch mode:"no-cors") can't be read,
  // so body is always empty. These are overwhelmingly fire-and-forget
  // tracking pixels / preconnect beacons — not API endpoints.
  if (opts && opts.responseType === "opaque") {
    return { kind: "asset", label: "opaque-cross-origin" };
  }
  // Server-declared content type (stripped to the bare MIME). Used only
  // as a weaker cross-check — magic bytes are authoritative; the header
  // is a server claim that can lie or be misconfigured.
  var declaredCt = null;
  if (opts && typeof opts.responseContentType === "string") {
    declaredCt = opts.responseContentType.toLowerCase().split(";")[0].trim() || null;
  }

  if (responseBody == null || responseBody === "") {
    return { kind: "empty", label: null };
  }
  if (responseBase64) {
    var bytes;
    try { bytes = base64ToUint8(responseBody); }
    catch (_) { return { kind: "api", label: null }; }
    if (bytes.length === 0) return { kind: "empty", label: null };
    var magic = sniffBinaryMagic(bytes);
    if (magic) {
      var note1 = declaredCt && declaredCt !== magic ? " (declared " + declaredCt + ")" : "";
      return { kind: "asset", label: magic + note1 };
    }
    // If the base64 decodes to printable text, run the text-asset sniff
    // too — misconfigured CDNs occasionally serve SVG as application/
    // octet-stream, triggering binary capture.
    try {
      var decoded = new TextDecoder("utf-8", { fatal: false }).decode(bytes);
      if (decoded) {
        var textMagic = _sniffTextAssetSignature(decoded);
        if (textMagic) {
          var note2 = declaredCt && declaredCt !== textMagic ? " (declared " + declaredCt + ")" : "";
          return { kind: "asset", label: textMagic + note2 };
        }
      }
    } catch (e) {
      // TextDecoder with fatal:false shouldn't throw on arbitrary bytes —
      // a throw here means the bytes input wasn't a valid Uint8Array shape.
      // Falls through to "binary-structured" classification.
      if (typeof console !== "undefined") console.debug("[discovery:classify] TextDecoder threw on binary bytes:", e && e.message || e);
    }
    // Base64 bytes with no magic match: could be protobuf, gRPC-Web, or any
    // structured binary format. These have schemas; don't skip learning.
    return { kind: "api", label: "binary-structured" };
  }
  // Text body. Sniff text-format assets first (SVG, CSS, HTML).
  var textAsset = _sniffTextAssetSignature(responseBody);
  if (textAsset) {
    var note3 = declaredCt && declaredCt !== textAsset ? " (declared " + declaredCt + ")" : "";
    return { kind: "asset", label: textAsset + note3 };
  }
  // Also run the binary sniff on raw bytes — servers sometimes ship
  // binary under a text content-type, which intercept captures as text.
  var probe = responseBody.length > 64 ? responseBody.slice(0, 64) : responseBody;
  var textBytes = new Uint8Array(probe.length);
  for (var i = 0; i < probe.length; i++) textBytes[i] = probe.charCodeAt(i) & 0xff;
  var magicText = sniffBinaryMagic(textBytes);
  if (magicText) {
    var note4 = declaredCt && declaredCt !== magicText ? " (declared " + declaredCt + ")" : "";
    return { kind: "asset", label: magicText + note4 };
  }
  // Asset content-types whose bodies have no unique structural prefix
  // (or where the @-rule sniff above misses common shapes). Trust the
  // server-declared MIME when the body is NOT a JSON root shape (`{`/`[`)
  // — JSON-shape under a JS MIME is JSONP/API data; under a CSS MIME it
  // wouldn't be valid CSS anyway, but the same gate keeps the path
  // symmetric. The set is restricted to MIMEs where servers have no
  // legitimate reason to ship API payloads (browsers execute JS, parse
  // CSS, render fonts — these aren't structured data formats).
  var ctAssetMimes = {
    "application/javascript": 1, "text/javascript": 1,
    "application/ecmascript": 1, "text/ecmascript": 1,
    "application/x-javascript": 1,
    "text/css": 1,
  };
  if (ctAssetMimes[declaredCt]) {
    var trimmed = responseBody.trimStart();
    var firstCh = trimmed.charCodeAt(0);
    // 0x7B = '{', 0x5B = '[' — JSON root shapes. Anything else starts
    // with a CSS selector / JS statement / comment and is asset content,
    // not API payload.
    if (firstCh !== 0x7B && firstCh !== 0x5B) {
      return { kind: "asset", label: declaredCt };
    }
  }
  return { kind: "api", label: null };
}

// Explicit self-binding for SW global scope — some extension loader configs
// don't hoist late function declarations into `self` reliably; attach so
// background.js can call them via importScripts.
if (typeof self !== "undefined") {
  self.sniffBinaryMagic = sniffBinaryMagic;
  self.classifyResponseAsset = classifyResponseAsset;
  self.deriveGraphQLMethodName = deriveGraphQLMethodName;
}

// ─── React Server Components (RSC) ─────────────────────────────────────────
//
// Next.js apps (Vercel, many modern SSR sites) stream RSC payloads with
// Content-Type `text/x-component`. Format is line-framed:
//
//    <id>:<payload>\n
//    <id>:<payload>\n
//    ...
//
// Where <id> is a hex integer and <payload> is one of:
//
//    I[moduleId, [chunks], exportName]       — module import reference
//    HL["href", ...]                          — preload link hint
//    E[errorId, "message"]                    — error
//    T<length>,<text>                         — text segment
//    S<id>:<name>                             — symbol reference ($Sreact.fragment)
//    <JSON>                                   — element tree or plain value
//
// Spec discussion: reactwg/server-components#5. Treated by the extension as
// a first-class protocol so schemas are learned from the JSON-tree rows
// instead of being garbage-merged as if the entire payload were one JSON
// body.

function isRSC(contentType) {
  if (!contentType) return false;
  const ct = contentType.toLowerCase().split(";")[0].trim();
  return ct === "text/x-component" || ct === "application/x-component";
}

// Quick body-sniff for RSC. Used when the server sends a generic
// Content-Type but the body clearly matches the line-framed shape.
function looksLikeRSC(bodyText) {
  if (!bodyText || typeof bodyText !== "string") return false;
  const head = bodyText.slice(0, 512);
  // First line must look like `<hex>:<payload>`.
  const m = head.match(/^[0-9a-f]+:/);
  if (!m) return false;
  // Second line should also match (helps avoid JSON `{"1":...}` false positives).
  const lines = head.split(/\r?\n/);
  if (lines.length < 2) return false;
  return /^[0-9a-f]+:/.test(lines[1]) || lines[1].length === 0;
}

// Parse one RSC row's payload into a typed record. Returns
// { type, value, raw } where type is one of:
//   "module" | "hint" | "error" | "text" | "symbol" | "json" | "unknown"
function _parseRSCPayload(payload) {
  if (typeof payload !== "string") return { type: "unknown", value: null, raw: payload };
  const p = payload;
  if (p.startsWith("I[")) {
    // I[moduleId, [chunks], exportName]
    try { return { type: "module", value: JSON.parse(p.slice(1)), raw: p }; }
    catch (_) { return { type: "module", value: null, raw: p }; }
  }
  if (p.startsWith("HL[")) {
    try { return { type: "hint", value: JSON.parse(p.slice(2)), raw: p }; }
    catch (_) { return { type: "hint", value: null, raw: p }; }
  }
  if (p.startsWith("E[")) {
    try { return { type: "error", value: JSON.parse(p.slice(1)), raw: p }; }
    catch (_) { return { type: "error", value: null, raw: p }; }
  }
  if (p.length >= 2 && p[0] === "T" && /[0-9]/.test(p[1])) {
    // T<hexLen>,<text>
    const commaIdx = p.indexOf(",");
    if (commaIdx > 0) {
      const hex = p.slice(1, commaIdx);
      if (/^[0-9a-f]+$/i.test(hex)) {
        return { type: "text", value: p.slice(commaIdx + 1), raw: p, length: parseInt(hex, 16) };
      }
    }
  }
  if (p.startsWith("\"$S") && p.endsWith("\"")) {
    return { type: "symbol", value: p.slice(3, -1), raw: p };
  }
  // Fall through: attempt JSON parse.
  try {
    const v = JSON.parse(p);
    return { type: "json", value: v, raw: p };
  } catch (_) {
    return { type: "unknown", value: null, raw: p };
  }
}

// Parse an RSC stream body.
// Returns { rows: [{id, type, value, raw, length?}], modules: [{moduleId, chunks, exportName}] }
// or null if the body isn't parseable as RSC.
function parseRSC(bodyText) {
  if (!bodyText || typeof bodyText !== "string") return null;
  if (!isRSC("text/x-component") && !looksLikeRSC(bodyText)) {
    // Caller will typically check isRSC(contentType) first; the second guard
    // is there for callers that feed us sniffed bodies.
  }
  const lines = bodyText.split(/\r?\n/);
  const rows = [];
  const modules = [];
  for (const line of lines) {
    if (!line) continue;
    const colon = line.indexOf(":");
    if (colon <= 0) continue;
    const idStr = line.slice(0, colon);
    if (!/^[0-9a-f]+$/i.test(idStr)) continue;
    const payload = line.slice(colon + 1);
    const parsed = _parseRSCPayload(payload);
    const row = { id: idStr, ...parsed };
    rows.push(row);
    if (parsed.type === "module" && Array.isArray(parsed.value)) {
      // [moduleId, [chunks], exportName]
      modules.push({
        moduleId: parsed.value[0],
        chunks: parsed.value[1] || [],
        exportName: parsed.value[2] || null,
      });
    }
  }
  if (rows.length === 0) return null;
  return { rows, modules };
}

if (typeof self !== "undefined") {
  self.isRSC = isRSC;
  self.looksLikeRSC = looksLikeRSC;
  self.parseRSC = parseRSC;
}
