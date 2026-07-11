/* Fetch Response + reply-body learning — the "learn from the server's reply" half of the fetch edge.
 *
 * A fetch resolves to a Response whose identity is concrete (ok/status/url) but whose body is opaque UNTIL a
 * concrete reply is known: r.json()/r.text() either resolve the cached body CONCOLIC (structure real, string/
 * bool leaves fork + carry their real example) or register the fetch so the provision re-run delivers it. This
 * is the richest source of real @H example values, so a consumed reply is always fetched. Borrows the reply
 * table + resolved-promise helper + fetch registrar from main.c (the scheduler side). */
#ifndef ENGINE_HOST_REPLY_H
#define ENGINE_HOST_REPLY_H

#include "quickjs.h"

/* Build a fetch Response for `url`: concrete identity + opaque body, with fromReply injection of a cached
   concrete body (from the reply table) so r.json()/r.text() then resolve the reply's real data concolic. */
JSValue make_response(JSContext *ctx, const char *url);
void pendreply_resolve(JSContext *ctx, const char *url, const char *body);   /* resolve every parked r.json()/r.text() of url with the concrete reply (qjs_provide) */
void pendreply_drain_opaque(JSContext *ctx);   /* finalize: resolve any never-delivered parked reply OPAQUE (shape) */

#endif
