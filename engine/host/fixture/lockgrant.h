/* THE LOCK-GRANT DOCUMENT: one lock requested (Web Locks §3.2.1 the request() method), whose callback reports
   the granted lock's name and mode and the held count §3.2.2 the query() method answers while the callback's
   promise is pending. §2.2 obtains the lock manager through Storage's bottle map named "web-locks".
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_LOCKGRANT_H
#define ENGINE_HOST_FIXTURE_LOCKGRANT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_LOCKGRANT[] =
    "<!doctype html><html><body>"
    "<script>"
    "navigator.locks.request('res', function (lock) {"
    "  return navigator.locks.query().then(function (s) {"
    "    fetch('/api/lock-granted?name=' + lock.name + '&mode=' + lock.mode + '&held=' + s.held.length);"
    "  });"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
