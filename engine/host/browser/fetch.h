/* The fetch() host edge — the primary request mechanism. Builds the endpoint record (method/url/params/
 * headers/body, with concolic URLs/bodies resolved to their real computed values) into the shared @H sink,
 * and returns a Response (make_response) so the fetch chain continues + r.json()/r.text() learn the reply.
 * Companion to xhr.c; both funnel into record_endpoint. Borrows the @H helpers from main.c (scheduler side). */
#ifndef ENGINE_HOST_FETCH_H
#define ENGINE_HOST_FETCH_H

#include "quickjs.h"

JSValue js_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

#endif
