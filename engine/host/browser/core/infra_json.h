/* Infra §6 "JSON": "parse a JSON string to a JavaScript value" is "Return ? Call(%JSON.parse%, undefined,
 * « string »)", and "parse JSON bytes to a JavaScript value" runs UTF-8 decode first. This component holds each
 * realm's %JSON.parse% and that decode; the Call itself is the caller's, made as a request from its own step
 * machine (step_call_run), so every parse a page's input reaches runs on the JSON.parse machine, which rests at
 * every completed value and is charged per byte by its tokenizer. */
#ifndef ENGINE_HOST_BROWSER_CORE_INFRA_JSON_H
#define ENGINE_HOST_BROWSER_CORE_INFRA_JSON_H

#include <stddef.h>
#include <stdint.h>

#include "quickjs.h"

void infra_json_init(JSContext *ctx);
void infra_json_free(void);

/* This realm's %JSON.parse%, captured before any of its scripts run, so a page replacing `JSON.parse` changes
   nothing the platform parses. OWNED. */
JSValue infra_json_parse_fn(JSContext *ctx);

/* "parse JSON bytes to a JavaScript value" step 1: "Let string be the result of running UTF-8 decode on
   bytes", as the String step 2 hands %JSON.parse%. OWNED, or JS_EXCEPTION with a throw live. */
JSValue infra_json_bytes_string(JSContext *ctx, const uint8_t *bytes, size_t len);

#endif
