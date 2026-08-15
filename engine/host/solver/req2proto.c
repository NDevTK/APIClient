/* Error-based schema learning — see req2proto.h for what an error envelope IS and why this is C. */
#include "solver/req2proto.h"

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/json_buf.h"

/* THE PROTOBUF TYPE NAMES ARE `descriptor.proto`'s OWN ENUM-VALUE NAMES — FieldDescriptorProto.Type — and the
   transcoder prints them verbatim into the violation description, so this is a decode of a wire vocabulary and
   not a guess at one. TYPE_GROUP, TYPE_MESSAGE and TYPE_ENUM are deliberately absent: a message field arrives
   as a `type.googleapis.com/…` URL instead (it has to, because the bare word would not say WHICH message), and
   an enum arrives the same way. A name not in this table and not a type URL is recorded as itself rather than
   collapsed to "unknown" — the server said something, and discarding it would be inventing an absence. */
static const struct { const char *proto, *json; } TYPE_NAMES[] = {
    { "TYPE_DOUBLE",   "double"   }, { "TYPE_FLOAT",    "float"    },
    { "TYPE_INT64",    "int64"    }, { "TYPE_UINT64",   "uint64"   },
    { "TYPE_INT32",    "int32"    }, { "TYPE_FIXED64",  "fixed64"  },
    { "TYPE_FIXED32",  "fixed32"  }, { "TYPE_BOOL",     "bool"     },
    { "TYPE_STRING",   "string"   }, { "TYPE_BYTES",    "bytes"    },
    { "TYPE_UINT32",   "uint32"   }, { "TYPE_SFIXED32", "sfixed32" },
    { "TYPE_SFIXED64", "sfixed64" }, { "TYPE_SINT32",   "sint32"   },
    { "TYPE_SINT64",   "sint64"   },
};

/* WHERE A SCHEMA CAME FROM. There is exactly one source today and the emit asserts it — see req2proto.h: this
   component has no entry that issues a request, so every record it holds was learned from a reply the PAGE's
   own code caused. A record that ever claims otherwise is a probe issuer someone added without the host edge
   §Attacker sources requires it to go out through. */
#define RPC_SRC_OBSERVED 1

typedef struct {
    char *name;          /* the violation's last path segment, `[i]` stripped */
    char *type;          /* a JSON type name, or the message type URL's own text */
    char *message_type;  /* `pkg.Message` for a type.googleapis.com/… field, else NULL */
    int   required;      /* a "Missing required field" violation named it */
    int   repeated;      /* the path segment carried `[i]` — error_details.proto's own notation */
} ProtoField;

typedef struct {
    char *key;           /* "<METHOD> <host><path>" — the endpoint identity, and the Send panel's lookup key */
    char *url;
    char *service;       /* ErrorInfo.metadata.service */
    char *method;        /* ErrorInfo.metadata.method */
    char **scopes; int nscopes, scap;   /* RFC 6750 §3's WWW-Authenticate `scope` parameter, split on spaces */
    ProtoField *fields; int nf, fcap;
    int source;
} Rpc;

static Rpc *g_rpcs;
static int  g_n, g_cap;

void req2proto_init(void) { g_rpcs = NULL; g_n = g_cap = 0; }

void req2proto_free(void) {
    for (int i = 0; i < g_n; i++) {
        Rpc *r = &g_rpcs[i];
        free(r->key); free(r->url); free(r->service); free(r->method);
        for (int j = 0; j < r->nscopes; j++) free(r->scopes[j]);
        free(r->scopes);
        for (int j = 0; j < r->nf; j++) { free(r->fields[j].name); free(r->fields[j].type); free(r->fields[j].message_type); }
        free(r->fields);
    }
    free(g_rpcs);
    g_rpcs = NULL; g_n = g_cap = 0;
}

int req2proto_count(void) { return g_n; }

/* ---- the record store -------------------------------------------------------------------------------- */

static char *dupz(const char *s) { char *r = strdup(s ? s : ""); CHECK(r, "req2proto: OOM copying a string"); return r; }

/* THE ENDPOINT IDENTITY, and it is the URL's HOST + PATH with the query dropped — a schema is a property of the
   method the path binds to, and `?key=…&alt=json` is not part of that binding. A URL with no scheme is used as
   it stands (the page's own relative request), because the alternative is this file inventing an origin. */
static char *identity_of(const char *method, const char *url) {
    const char *p = url, *scheme = strstr(url, "://");
    size_t n;
    char *key;
    if (scheme) p = scheme + 3;
    n = strcspn(p, "?#");
    key = malloc(strlen(method) + 2 + n);
    CHECK(key, "req2proto: OOM building an endpoint identity");
    memcpy(key, method, strlen(method));
    key[strlen(method)] = ' ';
    memcpy(key + strlen(method) + 1, p, n);
    key[strlen(method) + 1 + n] = 0;
    return key;
}

/* THE RETURNED POINTER IS VALID UNTIL THE NEXT CALL AND NOT ONE MOMENT LONGER — it names a slot in an array
   this function reallocs. Every caller takes one, fills it, and drops it inside a single req2proto_learn, which
   calls this exactly once; a second call held across the first would hand the caller freed storage. */
static Rpc *rpc_for(const char *method, const char *url) {
    char *key = identity_of(method, url);
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_rpcs[i].key, key)) { free(key); return &g_rpcs[i]; }
    if (g_n >= g_cap) {
        g_cap = g_cap ? g_cap * 2 : 8;
        g_rpcs = realloc(g_rpcs, (size_t)g_cap * sizeof(Rpc));
        CHECK(g_rpcs, "req2proto: OOM growing the learned-schema surface");
    }
    memset(&g_rpcs[g_n], 0, sizeof(Rpc));
    g_rpcs[g_n].key = key;
    g_rpcs[g_n].url = dupz(url);
    g_rpcs[g_n].source = RPC_SRC_OBSERVED;
    return &g_rpcs[g_n++];
}

/* A FIELD MERGES BY NAME, and by name ALONE, because a field NUMBER is not learnable from an organic reply.
   The number comes out of a violation's reflected VALUE, which is the field number only when the request body
   was the positional probe payload — so reading one here would file the page's own offending datum as a wire
   number. §RUN, DON'T MATCH: a value that is not computed is not emitted. The number arrives with the probe. */
static void field_merge(Rpc *r, const char *name, const char *type, const char *message_type,
                        int required, int repeated) {
    ProtoField *f = NULL;
    if (!name || !*name) return;
    for (int i = 0; i < r->nf; i++) if (!strcmp(r->fields[i].name, name)) { f = &r->fields[i]; break; }
    if (!f) {
        if (r->nf >= r->fcap) {
            r->fcap = r->fcap ? r->fcap * 2 : 8;
            r->fields = realloc(r->fields, (size_t)r->fcap * sizeof(ProtoField));
            CHECK(r->fields, "req2proto: OOM growing an endpoint's field list");
        }
        f = &r->fields[r->nf++];
        memset(f, 0, sizeof *f);
        f->name = dupz(name);
    }
    /* A LATER OBSERVATION REFINES, NEVER DOWNGRADES: a field seen once as required is required, once as
       repeated is repeated, and a named type supersedes no type at all. Two replies describing one field
       disagree only by having seen different parts of it. */
    if (type && *type && !f->type) f->type = dupz(type);
    if (message_type && *message_type && !f->message_type) f->message_type = dupz(message_type);
    if (required) f->required = 1;
    if (repeated) f->repeated = 1;
}

static void scope_add(Rpc *r, const char *s, size_t n) {
    if (!n) return;
    for (int i = 0; i < r->nscopes; i++)
        if (strlen(r->scopes[i]) == n && !memcmp(r->scopes[i], s, n)) return;
    if (r->nscopes >= r->scap) {
        r->scap = r->scap ? r->scap * 2 : 4;
        r->scopes = realloc(r->scopes, (size_t)r->scap * sizeof(char *));
        CHECK(r->scopes, "req2proto: OOM growing an endpoint's scope list");
    }
    r->scopes[r->nscopes] = malloc(n + 1);
    CHECK(r->scopes[r->nscopes], "req2proto: OOM copying an OAuth scope");
    memcpy(r->scopes[r->nscopes], s, n);
    r->scopes[r->nscopes][n] = 0;
    r->nscopes++;
}

/* ---- the violation description ----------------------------------------------------------------------- */

/* `Invalid value at 'FIELD.PATH' (TYPE), <the offending value>` — the transcoder's own rejection sentence. Only
   the first two parts are read: the third is the value the SERVER echoed back, which for a reply the page's own
   code caused is the page's data and for a probe reply is the field number, and this component never sees the
   second kind (req2proto.h). `*ptype` and `*ptype_n` name the type text in place; the return is the field path,
   or NULL when the sentence is not this one. */
static const char *violation_parse(const char *desc, size_t *ppath_n, const char **ptype, size_t *ptype_n) {
    static const char AT[] = "Invalid value at '";
    const char *at = strstr(desc, AT), *path, *close, *open, *tclose;
    if (!at) return NULL;
    path = at + sizeof AT - 1;
    close = strchr(path, '\'');
    if (!close) return NULL;
    open = close + 1;
    while (*open == ' ') open++;
    if (*open != '(') return NULL;
    tclose = strchr(open + 1, ')');
    if (!tclose) return NULL;
    *ppath_n = (size_t)(close - path);
    *ptype = open + 1;
    *ptype_n = (size_t)(tclose - open - 1);
    return path;
}

/* THE LAST SEGMENT OF A VIOLATION PATH, with `[i]` stripped and the index reported as REPEATEDNESS.
   error_details.proto states the notation itself — `emailAddresses[2].type[1]` — so this is the format's own
   grammar rather than a pattern noticed in a sample. */
static char *leaf_of(const char *path, size_t n, int *prepeated) {
    size_t end = n, start;
    char *out;
    *prepeated = 0;
    if (end && path[end - 1] == ']') {
        size_t b = end;
        while (b && path[b - 1] != '[') b--;
        if (b) { end = b - 1; *prepeated = 1; }
    }
    start = end;
    while (start && path[start - 1] != '.') start--;
    out = malloc(end - start + 1);
    CHECK(out, "req2proto: OOM taking a violation path's leaf");
    memcpy(out, path + start, end - start);
    out[end - start] = 0;
    return out;
}

/* `Missing required field NAME at 'PARENT'` — the other sentence, and the only source of REQUIRED there is.
   Returns the name (owned) or NULL. */
static char *required_name_of(const char *desc) {
    static const char PRE[] = "Missing required field ";
    const char *p, *end;
    char *out;
    if (strncmp(desc, PRE, sizeof PRE - 1)) return NULL;
    p = desc + sizeof PRE - 1;
    end = strstr(p, " at '");
    if (!end) end = p + strlen(p);
    while (end > p && end[-1] == ' ') end--;
    if (end == p) return NULL;
    out = malloc((size_t)(end - p) + 1);
    CHECK(out, "req2proto: OOM taking a required field name");
    memcpy(out, p, (size_t)(end - p));
    out[end - p] = 0;
    return out;
}

/* ---- reading the reply record ------------------------------------------------------------------------ */

static char *prop_str(JSContext *ctx, JSValueConst o, const char *k) {
    JSValue v;
    const char *s;
    char *out = NULL;
    if (!JS_IsObject(o)) return NULL;
    v = JS_GetPropertyStr(ctx, o, k);
    if (JS_IsException(v)) { JS_FreeValue(ctx, JS_GetException(ctx)); return NULL; }
    if (JS_IsString(v) && (s = JS_ToCString(ctx, v)) != NULL) { out = dupz(s); JS_FreeCString(ctx, s); }
    JS_FreeValue(ctx, v);
    return out;
}

static int prop_len(JSContext *ctx, JSValueConst o, int *pn) {
    JSValue v;
    int32_t n = 0;
    if (!JS_IsObject(o)) return 0;
    v = JS_GetPropertyStr(ctx, o, "length");
    if (JS_IsException(v)) { JS_FreeValue(ctx, JS_GetException(ctx)); return 0; }
    if (JS_ToInt32(ctx, &n, v) < 0) { JS_FreeValue(ctx, JS_GetException(ctx)); JS_FreeValue(ctx, v); return 0; }
    JS_FreeValue(ctx, v);
    *pn = (int)n;
    return 1;
}

/* RFC 6750 §3: `WWW-Authenticate: Bearer realm="…", scope="a b c"` — the scopes a caller must hold, stated by
   the server that refused for the want of them. Read wherever the header is present rather than only on a 403,
   because the header's presence IS the statement and its status code is not the thing being read. */
static void learn_scopes(Rpc *r, const char *www_auth) {
    static const char K[] = "scope=\"";
    const char *p = strstr(www_auth, K), *end, *tok;
    if (!p) return;
    p += sizeof K - 1;
    end = strchr(p, '"');
    if (!end) return;
    for (tok = p; tok < end; ) {
        const char *e = tok;
        while (e < end && *e != ' ' && *e != '\t') e++;
        scope_add(r, tok, (size_t)(e - tok));
        while (e < end && (*e == ' ' || *e == '\t')) e++;
        tok = e;
    }
}

/* The reply record's header list is `[[name, value], …]` — the shape every host of this engine builds. */
static char *header_of(JSContext *ctx, JSValueConst reply, const char *want) {
    JSValue hdrs;
    char *out = NULL;
    int n = 0;
    if (!JS_IsObject(reply)) return NULL;
    hdrs = JS_GetPropertyStr(ctx, reply, "headers");
    if (JS_IsException(hdrs)) { JS_FreeValue(ctx, JS_GetException(ctx)); return NULL; }
    if (prop_len(ctx, hdrs, &n)) {
        for (int i = 0; i < n && !out; i++) {
            JSValue pair = JS_GetPropertyUint32(ctx, hdrs, (uint32_t)i);
            JSValue nv = JS_IsObject(pair) ? JS_GetPropertyUint32(ctx, pair, 0) : JS_UNDEFINED;
            JSValue vv = JS_IsObject(pair) ? JS_GetPropertyUint32(ctx, pair, 1) : JS_UNDEFINED;
            const char *ns = JS_IsString(nv) ? JS_ToCString(ctx, nv) : NULL;
            const char *vs = JS_IsString(vv) ? JS_ToCString(ctx, vv) : NULL;
            if (ns && vs) {
                size_t k;
                int same = 1;
                for (k = 0; want[k] && ns[k]; k++) {
                    char a = ns[k], b = want[k];
                    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                    if (a != b) { same = 0; break; }
                }
                if (same && !want[k] && !ns[k]) out = dupz(vs);
            }
            if (ns) JS_FreeCString(ctx, ns);
            if (vs) JS_FreeCString(ctx, vs);
            JS_FreeValue(ctx, nv);
            JS_FreeValue(ctx, vv);
            JS_FreeValue(ctx, pair);
        }
    }
    JS_FreeValue(ctx, hdrs);
    return out;
}

/* One `google.rpc.BadRequest` detail: its `fieldViolations` are the schema. */
static void learn_violations(JSContext *ctx, Rpc *r, JSValueConst detail) {
    JSValue list = JS_GetPropertyStr(ctx, detail, "fieldViolations");
    int n = 0;
    if (JS_IsException(list)) { JS_FreeValue(ctx, JS_GetException(ctx)); return; }
    if (prop_len(ctx, list, &n)) {
        for (int i = 0; i < n; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, list, (uint32_t)i);
            char *field = prop_str(ctx, v, "field");
            char *desc  = prop_str(ctx, v, "description");
            char *req_name;
            if (desc && (req_name = required_name_of(desc)) != NULL) {
                field_merge(r, req_name, NULL, NULL, /*required*/1, /*repeated*/0);
                free(req_name);
            } else if (desc) {
                size_t path_n = 0, type_n = 0;
                const char *type = NULL;
                /* THE VIOLATION'S OWN `field` IS THE PATH, and the sentence's is a fallback for the servers
                   that fill only one of the two. They are the same path; error_details.proto defines `field`
                   and the transcoder repeats it inside `description`. */
                const char *path = violation_parse(desc, &path_n, &type, &type_n);
                if (path) {
                    int repeated = 0;
                    char *leaf = field ? leaf_of(field, strlen(field), &repeated)
                                       : leaf_of(path, path_n, &repeated);
                    char *tname = NULL, *mtype = NULL;
                    static const char ANY[] = "type.googleapis.com/";
                    if (type_n > sizeof ANY - 1 && !memcmp(type, ANY, sizeof ANY - 1)) {
                        mtype = malloc(type_n - (sizeof ANY - 1) + 1);
                        CHECK(mtype, "req2proto: OOM copying a message type");
                        memcpy(mtype, type + sizeof ANY - 1, type_n - (sizeof ANY - 1));
                        mtype[type_n - (sizeof ANY - 1)] = 0;
                        tname = dupz("message");
                    } else {
                        for (size_t t = 0; t < sizeof TYPE_NAMES / sizeof TYPE_NAMES[0]; t++)
                            if (strlen(TYPE_NAMES[t].proto) == type_n && !memcmp(TYPE_NAMES[t].proto, type, type_n)) {
                                tname = dupz(TYPE_NAMES[t].json);
                                break;
                            }
                        if (!tname && type_n) {
                            tname = malloc(type_n + 1);
                            CHECK(tname, "req2proto: OOM copying a field type");
                            memcpy(tname, type, type_n);
                            tname[type_n] = 0;
                        }
                    }
                    field_merge(r, leaf, tname, mtype, /*required*/0, repeated);
                    free(leaf); free(tname); free(mtype);
                }
            }
            free(field);
            free(desc);
            JS_FreeValue(ctx, v);
        }
    }
    JS_FreeValue(ctx, list);
}

/* One `google.rpc.ErrorInfo` detail: its `metadata` map is where the transcoder states the CANONICAL service
   and method, which is the fact a URL path cannot give and the whole reason this detail is read. */
static void learn_error_info(JSContext *ctx, Rpc *r, JSValueConst detail) {
    JSValue md = JS_GetPropertyStr(ctx, detail, "metadata");
    char *service, *method;
    if (JS_IsException(md)) { JS_FreeValue(ctx, JS_GetException(ctx)); return; }
    service = prop_str(ctx, md, "service");
    method  = prop_str(ctx, md, "method");
    if (service && *service && !r->service) r->service = service; else free(service);
    if (method  && *method  && !r->method)  r->method  = method;  else free(method);
    JS_FreeValue(ctx, md);
}

void req2proto_learn(JSContext *ctx, const char *method, const char *url, JSValueConst reply) {
    char *body, *www_auth;
    JSValue parsed, err, details;
    int n = 0;
    Rpc *r;

    /* THE IDENTITY IS HALF THE FINDING. A schema filed under the wrong method is a request body the Send panel
       would offer at an endpoint that never takes one, so the caller states both rather than defaulting either
       — the pending entry it reads them off carries both. */
    DCHECK(method != NULL && *method, "a reply was learned from with no request METHOD — the schema is filed "
           "under the endpoint identity `<METHOD> <host><path>`, and a method-less key collides every verb of "
           "one path into one record");
    DCHECK(url != NULL && *url, "a reply was learned from with no request URL");
    /* A NETWORK ERROR IS NOT AN ENVELOPE. JS_NULL is Fetch §5.6's network error and carries no reply at all;
       the caller hands it over unchanged rather than deciding here, which is why this is a shape test and not
       an `if (!reply) return` past a broken invariant. */
    if (!JS_IsObject(reply)) return;

    body = prop_str(ctx, reply, "body");
    if (!body) return;
    /* THE REAL CODEC, RUN — §A JS-engine encoding builtin is modeled FAITHFULLY, never re-implemented. Most
       replies are not JSON at all (an HTML error page, a script, an image), and that is an ordinary fact about
       the web rather than an engine invariant: the exception is TAKEN and dropped here because there is nothing
       to report, exactly as the fetch drain drops a rejected delivery the page owns. */
    parsed = JS_ParseJSON(ctx, body, strlen(body), "<reply>");
    free(body);
    if (JS_IsException(parsed)) { JS_FreeValue(ctx, JS_GetException(ctx)); return; }

    err = JS_IsObject(parsed) ? JS_GetPropertyStr(ctx, parsed, "error") : JS_UNDEFINED;
    if (JS_IsException(err)) { JS_FreeValue(ctx, JS_GetException(ctx)); err = JS_UNDEFINED; }
    if (!JS_IsObject(err)) { JS_FreeValue(ctx, err); JS_FreeValue(ctx, parsed); return; }

    r = rpc_for(method, url);

    details = JS_GetPropertyStr(ctx, err, "details");
    if (JS_IsException(details)) { JS_FreeValue(ctx, JS_GetException(ctx)); details = JS_UNDEFINED; }
    if (prop_len(ctx, details, &n)) {
        for (int i = 0; i < n; i++) {
            JSValue d = JS_GetPropertyUint32(ctx, details, (uint32_t)i);
            /* `details` is `repeated google.protobuf.Any`, so each member names its own type in `@type` — that
               is the JSON mapping of Any and the only way to know which detail this is. */
            char *at = prop_str(ctx, d, "@type");
            if (at && strstr(at, "BadRequest")) learn_violations(ctx, r, d);
            if (at && strstr(at, "ErrorInfo"))  learn_error_info(ctx, r, d);
            free(at);
            JS_FreeValue(ctx, d);
        }
    }
    JS_FreeValue(ctx, details);
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, parsed);

    www_auth = header_of(ctx, reply, "www-authenticate");
    if (www_auth) { learn_scopes(r, www_auth); free(www_auth); }
}

/* ---- the emit ------------------------------------------------------------------------------------------ */

char *req2proto_json_object(void) {
    JsonBuf b = { 0 };
    json_buf_puts(&b, "{");
    for (int i = 0; i < g_n; i++) {
        Rpc *r = &g_rpcs[i];
        /* THE PROVENANCE, ASSERTED WHERE IT LEAVES. §Attacker sources forbids firing a state-mutating request
           to learn, and this component honours that by having no entry that issues one — so the only source a
           record can carry is an observed reply. A record claiming anything else means an issuer was added
           without the page-context host edge that rule requires it to go out through, and the finding would
           reach the reviewer as something this tool actively probed for. */
        DCHECK(r->source == RPC_SRC_OBSERVED,
               "a learned schema carries a provenance this component cannot produce — it reads replies and "
               "issues nothing, so a record from any other source is a probe issuer built without the "
               "page-context edge §Attacker sources requires a state-mutating probe to leave through");
        if (i) json_buf_puts(&b, ",");
        json_buf_str(&b, r->key);
        json_buf_puts(&b, ":{\"url\":");
        json_buf_str(&b, r->url);
        /* EACH OF THE THREE IS EMITTED ONLY WHEN IT WAS LEARNED. An absent `service` is "the server never said",
           which a consumer reads as one; an empty string would be a name, and an empty `scopes` array would say
           the call needs none. */
        if (r->service) { json_buf_puts(&b, ",\"service\":"); json_buf_str(&b, r->service); }
        if (r->method)  { json_buf_puts(&b, ",\"method\":");  json_buf_str(&b, r->method); }
        if (r->nscopes) {
            json_buf_puts(&b, ",\"scopes\":[");
            for (int j = 0; j < r->nscopes; j++) { if (j) json_buf_puts(&b, ","); json_buf_str(&b, r->scopes[j]); }
            json_buf_puts(&b, "]");
        }
        json_buf_puts(&b, ",\"fields\":{");
        for (int j = 0; j < r->nf; j++) {
            ProtoField *f = &r->fields[j];
            if (j) json_buf_puts(&b, ",");
            json_buf_str(&b, f->name);
            json_buf_puts(&b, ":{\"type\":");
            json_buf_str(&b, f->type ? f->type : "unknown");
            json_buf_puts(&b, ",\"required\":");
            json_buf_puts(&b, f->required ? "true" : "false");
            json_buf_puts(&b, ",\"label\":");
            json_buf_str(&b, f->repeated ? "repeated" : f->required ? "required" : "optional");
            if (f->message_type) { json_buf_puts(&b, ",\"messageType\":"); json_buf_str(&b, f->message_type); }
            json_buf_puts(&b, "}");
        }
        json_buf_puts(&b, "}}");
    }
    json_buf_puts(&b, "}");
    return json_buf_take(&b);
}
