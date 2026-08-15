/* The Headers interface — WHATWG Fetch §5, and the HEADER LIST behind it. See headers.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_HEADERS_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_HEADERS_H
#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_iter.h"

/* §5.1's header list: (name, value) pairs, names lowercased on the way in and never combined into one entry —
   `Set-Cookie` is the reason the spec keeps them separate, and `getSetCookie` is the member that reads them
   back. It is public because two things need to build one without a JS object in sight: the Headers interface
   itself, and `fetch`'s conversion of an `init.headers` bag into the set an endpoint requires. */
typedef struct { char *name; char *value; } HeaderEntry;
typedef struct { HeaderEntry *e; int n, cap; } HeaderList;

/* §5.1: "A Headers object has an associated GUARD" — which of the page's writes the object refuses, and how.
   "none" refuses nothing (a page's own `new Headers()`); "immutable" THROWS on every write (a Response the
   engine handed the page); "request"/"request-no-cors" and "response" SILENTLY drop the names the browser
   owns. The three answers are distinct in the spec and must stay distinct here. */
typedef enum {
    HEADERS_GUARD_NONE = 0,
    HEADERS_GUARD_IMMUTABLE,
    HEADERS_GUARD_REQUEST,
    HEADERS_GUARD_REQUEST_NO_CORS,
    HEADERS_GUARD_RESPONSE,
} HeadersGuard;


/* §5.1's GRAMMAR, exported because a SECOND standard performs the same three tests and throws DIFFERENT
   errors for them: XHR §3.5.2 setRequestHeader() answers a bad name or value with a "SyntaxError"
   DOMException where §5.1's own members answer with a TypeError, and it answers a forbidden request-header
   with a silent return. So the grammar is stated once and the error stays each standard's own — a second copy
   of "what a header name is" is a second thing to keep in step with RFC 7230.
     `header_name_valid` is "a header name is a NAME" (a token).
     `header_value_normalize_valid` is "normalize a header value" followed by "a header value is a VALUE": it
   strips leading and trailing HTTP whitespace and then refuses NUL/CR/LF, returning the normalized value
   (caller frees, `*pn` is its length) or NULL for a value that is not one.
     `header_forbidden_request` is "forbidden request-header", over a LOWERCASED name. */
bool  header_name_valid(const char *name, size_t len);
char *header_value_normalize_valid(const char *value, size_t len, size_t *pn);
bool  header_forbidden_request(const char *lower_name, const char *value);

void  header_list_free(HeaderList *l);
/* §5.1 append: lowercase the name, keep the pair. Both strings are COPIED. */
void  header_list_append(HeaderList *l, const char *name, const char *value);
/* §5.1 set: replace every entry with this name by one. */
void  header_list_set(HeaderList *l, const char *name, const char *value);
void  header_list_delete(HeaderList *l, const char *name);
/* §2.2.4 "get a structured header value": the entries with this name, joined by ", " (malloc'd; NULL when the
   list has none, which is what `get` returns null for). */
char *header_list_get(const HeaderList *l, const char *name);

/* §5.1 "fill", AS A SUB-SEQUENCE. Converting a `HeadersInit` is [[OwnPropertyKeys]] and then a [[Get]] and a
   ToString per key — every one of them the page's code on a Proxy or an accessor, so every one of them a
   request. It lives outside the Headers constructor because `fetch(u, {headers: ...})` performs the SAME
   conversion and the spec states it once; a machine performing it embeds this cursor, declares it in its own
   `visit` (headers_fill_visit); the driver's teardown discharges that one declaration.
     Returns >0 (the caller returns it), 0 when the list is filled, or -1 with a throw live. */

typedef struct {
    uint8_t     phase;
    JSValue     name, value;    /* owned: the pair being converted */
    IterCursor  outer, inner;   /* the sequence arm: the init's pairs, and the two items of one pair */
    RecordCursor rec;           /* the record arm */
    JSValue     item[2];        /* owned: the pair's items as they arrive */
    int         nitem;
} HeadersFill;
void headers_fill_init(HeadersFill *f);
void headers_fill_visit(JSContext *ctx, HeadersFill *f, JSStepVisit *v);
int  headers_fill_run(JSContext *ctx, JSStepHdr *h, HeadersFill *f, JSValueConst init, HeaderList *out,
                      HeadersGuard guard, JSValue in, JSValue **out_cb, int *out_argc);

void    headers_init(JSContext *ctx);                       /* register the class + its machines (install time) */
void    headers_install_proto(JSContext *ctx);              /* §5.1's prototype, for ONE realm */
void    headers_install(JSContext *ctx, JSValueConst global);   /* the Headers interface object */
void    headers_free(JSContext *ctx);   /* the prototypes this component holds */
/* A Headers over an existing list; the object takes a COPY, because a header list a component owns outlives
   nothing the page can reach and a page must not be able to mutate a reply's headers through the copy it was
   handed. */
JSValue headers_new(JSContext *ctx, const HeaderList *src, HeadersGuard guard);
/* The list behind a Headers object, or NULL when `v` is not one — how `fetch` reads a `new Headers(...)` it was
   handed without going back through the page's own accessors. */
const HeaderList *headers_list_of(JSValueConst v);
/* The GUARD of a Headers object — what a clone of the thing holding it must give its own copy. It is the
   object's state, so it cannot be recovered from the list. */
HeadersGuard headers_guard_of(JSValueConst v);

#endif
