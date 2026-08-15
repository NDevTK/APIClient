/* ERROR-BASED SCHEMA LEARNING — an API's own rejection is a description of the request it wanted.
 *
 * WHAT THIS IS. A gRPC/protobuf-transcoded API that refuses a request answers with `google.rpc.Status`
 * (status.proto: `{"error":{"code","message","details":[Any…]}}`), and the `details` carry
 * `google.rpc.BadRequest` — a list of `FieldViolation{field, description}` — beside `google.rpc.ErrorInfo`,
 * whose `metadata` map is where Google's transcoder puts the canonical `service` and `method`. The violation's
 * `field` is a dot-separated path whose repeated segments carry `[i]` (error_details.proto's own example:
 * `emailAddresses[2].type[1]`), and its `description` names the field's protobuf TYPE by the enum-value name
 * `descriptor.proto` defines (`TYPE_STRING`, `TYPE_INT64`, `TYPE_MESSAGE`, …) or by a `type.googleapis.com/…`
 * message URL. A 403 additionally carries the OAuth scopes the call needs in `WWW-Authenticate`'s `scope`
 * parameter (RFC 6750 §3, space-delimited). Read together those four facts ARE the endpoint's request schema:
 * field names, field NUMBERS, types, repeated/required labels, the message types of nested fields, the service
 * and method the path really binds to, and the scopes a caller must hold.
 *
 * WHY IT IS THE ENGINE'S. It was `extension/lib/req2proto.js`, 977 lines of semantics in the untrusted-adjacent
 * host — CLAUDE.md §Architecture names it by name as JS that must become a C component, and every reply it read
 * is a reply this engine already holds. Nothing here talks to the network: the component reads a reply record
 * the trusted zone fetched through the one safeFetch chokepoint and delivered, which is the only shape
 * SECURITY.md leaves for it and the reason no policy `if` appears below (a safety `if` inside the engine is a
 * layering violation).
 *
 * WHAT IT DELIBERATELY DOES NOT DO. §Attacker sources: "A state-mutating request (POST/PUT/DELETE, or a
 * side-effecting GET) is NEVER fired to learn." So this component has NO ENTRY THAT ISSUES A REQUEST — the rule
 * is structural here rather than a check someone remembers to write, and the only reply it can ever see is one
 * the PAGE'S OWN code caused. That is also why every record it keeps is stamped with that provenance and why
 * the emit asserts it: a schema this engine merely OBSERVED must never leave here looking like one it probed
 * for. Deriving and issuing the probe is the next subproblem in this file, and it is blocked on a host edge
 * that does not exist — the pending seam carries a URL and the chokepoint performs a GET (SECURITY.md §Network,
 * "GET only"), so an error probe has to be fired AS THE PAGE through pageContextFetch, which the engine has no
 * edge to. It is not started here, because a probe that silently becomes a GET would file the answer to a
 * question nobody asked under the identity of the one that was.
 *
 * THE IDENTITY IS THE ENDPOINT'S. A record is keyed `"<METHOD> <host><path>"` — the same key the endpoint
 * surface is grouped by and the same one the Send panel resolves a body schema with, so nothing between here
 * and there re-keys anything. */
#ifndef ENGINE_HOST_SOLVER_REQ2PROTO_H
#define ENGINE_HOST_SOLVER_REQ2PROTO_H

#include "quickjs.h"

void req2proto_init(void);
void req2proto_free(void);

/* LEARN FROM ONE REPLY. `method` and `url` are the request's, read off the pending entry the reply settles;
   `reply` is the host's reply record (`{status, statusText, headers:[[name,value]…], body, urlList}`) or
   JS_NULL for a network error. A reply that is not an error envelope teaches nothing and is not an error —
   most replies are not one, which is why this has no return value to test. Idempotent: two flows parked on the
   same URL learn the same schema, and the merge is by field NUMBER (the one identity a protobuf field has). */
void req2proto_learn(JSContext *ctx, const char *method, const char *url, JSValueConst reply);

/* THE LEARNED SURFACE as a malloc'd JSON OBJECT (caller frees), keyed by endpoint identity:
     {"POST host/path": {"url":…, "service":…, "method":…, "scopes":[…], "fieldCount":N,
                         "fields": {"<name>": {"type","number","label","required","messageType"}}}}
   The `fields` MAP and its member shape are the Send panel's existing contract (`lib/send.js` reads
   `probeResult.fields` as name -> {type, number, required, label, messageType, children}), so the engine emits
   the record the consumer already reads rather than a new vocabulary a host would have to translate. `{}` when
   nothing was learned, which is a positive statement and not a hole. */
char *req2proto_json_object(void);

/* How many endpoints have a learned schema — the emit's own count, and what a caller asserts against. */
int req2proto_count(void);

#endif
