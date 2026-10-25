/* solver/request_observe.c — a request observed leaving the engine (see request_observe.h). */
#include "solver/request_observe.h"
#include "solver/flow.h"
#include "solver/solve.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

/* Is `domain` the marker host or a name under it? */
static int host_is_or_under(const char *domain, const char *marker)
{
    size_t dn = strlen(domain), mn = strlen(marker);

    if (dn == mn) return strcmp(domain, marker) == 0;
    return dn > mn && domain[dn - mn - 1] == '.' && strcmp(domain + dn - mn, marker) == 0;
}

int request_observed(const FetchRequest *req, const char *method, RequestPiece piece, const char *header,
                     const char *marker)
{
    DCHECK(req && req->url && req->method && marker && *marker,
           "a request observation was asked with no request, no URL, no method or no marker");
    DCHECK((piece == REQUEST_PIECE_HEADER) == (header != NULL),
           "a request observation named a header for a piece that is not one, or the header piece with no name");
    if (method && strcmp(method, req->method) != 0) return 0;
    switch (piece) {
    case REQUEST_PIECE_HOST: {
        UrlRecord u;
        int hit;

        url_record_init(&u);
        /* The URL leaving the engine is the serialization §4.1 step 6 produced, so it parses. */
        CHECKF(url_parse(&u, req->url, strlen(req->url), NULL),
               "a request left the engine with the URL `%s`, which does not parse", req->url);
        hit = u.host.kind == URL_HOST_DOMAIN && host_is_or_under(u.host.domain, marker);
        url_record_free(&u);
        return hit;
    }
    case REQUEST_PIECE_ADDRESS:
        return strstr(req->url, marker) != NULL;
    case REQUEST_PIECE_HEADER: {
        char *v = req->headers ? header_list_get(req->headers, header) : NULL;
        int hit = v && strstr(v, marker) != NULL;

        free(v);
        return hit;
    }
    case REQUEST_PIECE_BODY:
        return req->body && memmem(req->body, req->body_len, marker, strlen(marker)) != NULL;
    }
    DFAIL("a request observation named a piece outside RequestPiece");
    return 0;
}

void request_observe_park(const FetchRequest *req)
{
    const Flow *f = flow_running();

    if (!f || !f->cand_src) return;
    /* A sink candidate's class decides what its request proves (solve.c's request class). A witness verification
       observes at the mint today (solver/endpoint.c), so it is not read here. */
    if (f->cand_sink) solve_request_parked(req);
}
