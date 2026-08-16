/* FETCH §2.9 PORT BLOCKING — "should request be blocked due to a bad port?" and the table it reads. See
   port_blocking.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_PORT_BLOCKING_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_PORT_BLOCKING_H
#include "core/url/url.h"

/* §2.9's TWO ANSWERS, named as the algorithm names them rather than as a bool. "Return blocked" and "return
   allowed" are the spec's own words, and a caller that reads `fetch_block_bad_port(&rec)` as a bare truth value
   has a fifty-percent chance of reading the polarity backwards with nothing to say so — a fetch to :25 that
   proceeds and a fetch to :443 that is refused are both silent. */
typedef enum {
    FETCH_PORT_ALLOWED = 0,
    FETCH_PORT_BLOCKED = 1,
} FetchPortVerdict;

/* §2.9: "To determine whether fetching a request request should be blocked due to a bad port". The parameter is
   the request's CURRENT URL as a parsed record, because that is what the algorithm reads — a scheme and a port,
   both of which only the URL parser can answer (`http://x:0080/`'s port is 80, and `https://x:443/`'s port is
   null because the parser drops a scheme's default). `url` is non-null and carries a scheme: the record comes
   from a parse that SUCCEEDED, which is the caller's obligation and this file's first two DCHECKs. */
FetchPortVerdict fetch_block_bad_port(const UrlRecord *url);

#endif
