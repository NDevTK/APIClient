/* THE INTL DOCUMENT: the environment ECMAScript leaves to the implementation, answered by the Intl host. Date's
   local time is the pinned system time zone (core/intl/intl_environment.h's UTC), whatever zone the operating
   system the engine runs on is set to. One of test_forced.c's exploring documents (TF_DOCS); it ends with
   TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_INTL_H
#define ENGINE_HOST_FIXTURE_INTL_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_INTL[] =
    "<!doctype html><html><body>"
    "<script>"
    "var lt = new Date(Date.UTC(2024, 6, 2, 3, 4));"
    "fetch('/api/intltz?off=' + lt.getTimezoneOffset() + '&h=' + lt.getHours() + '&m=' + lt.getMinutes() +"
    " '&set=' + new Date(2024, 0, 2, 3).getTime());"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
