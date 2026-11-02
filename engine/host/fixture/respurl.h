/* THE RESPONSE-URL DOCUMENT: Fetch §5.5's `url` getter over the reply to a request whose address the query names.
   The address is unknown input, so the engine fetches nothing for it, and on the arm where it reaches another
   origin in `cors` mode the reply is one no server sent (core/fetch/fetch.h's fetch_reply_unaddressed). Its URL
   list holds the request's current URL, an unknown URL record, so `r.url` is the serialization of that record
   and carries the query's provenance into the request built from it.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RESPURL_H
#define ENGINE_HOST_FIXTURE_RESPURL_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RESPURL[] =
    "<!doctype html><html><body>"
    "<script>"
    "(async function () {"
    "  var q = location.search, at = q.indexOf('ru=');"
    "  if (at < 0) return;"
    "  var r = await fetch(decodeURIComponent(q.slice(at + 3).split('&')[0]));"
    "  var u = r.url;"
    "  fetch('/api/respurl-read?redirected=' + r.redirected);"
    "  fetch('/api/respurl-url?u=' + encodeURIComponent(u));"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
