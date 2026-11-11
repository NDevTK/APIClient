/* The sale documents: a page that forks on `location.hash`, where one arm asks for a reply and reports the
   reply's length once it has it, and the other arm holds the thread for many quanta before it reports. At the
   payment whose bill first lists the owed request, test_forced.c's provider sells every member but the one
   switched in (tf_sale_act), so the member owing the request is relocated before its reply is written. The
   member's report is reached only if it returns within the session.
   `sale`: the arm against the example owes the request and the page's own run is busy. `saleown`: the page's own
   run owes it and the arm against the example is busy. `salectl`: `sale`'s shape with no act, the control.
   Three of test_forced.c's exploring documents (TF_DOCS); each ends with TF_CANVAS_INK, which test_forced.c
   defines before including this file. */
#ifndef ENGINE_HOST_FIXTURE_SALE_H
#define ENGINE_HOST_FIXTURE_SALE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

/* The busy arm's loop: enough work that the arm is switched in at several consecutive payments. */
#define TF_SALE_BUSY "var slN = 0; for (var slI = 0; slI < 200000; slI++) slN += slI & 1;"

static const char HTML_SALE[] =
    "<!doctype html><html><body>"
    "<script>"
    "var slForced = location.hash === '#owe';"
    "if (slForced) {"
    "  fetch('/api/sl-forced-owed').then(function (r) { return r.text(); })"
    "    .then(function (t) { fetch('/api/sl-forced-done?b=' + t.length); });"
    "} else {"
    TF_SALE_BUSY
    "  fetch('/api/sl-forced-busy?n=' + slN);"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

static const char HTML_SALEOWN[] =
    "<!doctype html><html><body>"
    "<script>"
    "var slOwn = location.hash === '#busy';"
    "if (slOwn) {"
    TF_SALE_BUSY
    "  fetch('/api/sl-own-busy?n=' + slN);"
    "} else {"
    "  fetch('/api/sl-own-owed').then(function (r) { return r.text(); })"
    "    .then(function (t) { fetch('/api/sl-own-done?b=' + t.length); });"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

static const char HTML_SALECTL[] =
    "<!doctype html><html><body>"
    "<script>"
    "var slCtl = location.hash === '#owe';"
    "if (slCtl) {"
    "  fetch('/api/sl-ctl-owed').then(function (r) { return r.text(); })"
    "    .then(function (t) { fetch('/api/sl-ctl-done?b=' + t.length); });"
    "} else {"
    TF_SALE_BUSY
    "  fetch('/api/sl-ctl-busy?n=' + slN);"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
