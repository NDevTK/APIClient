/* The sale documents: a page that forks on `location.hash`, where one arm asks for a reply and reports the
   reply's length once it has it, and the other arm holds the thread for many quanta before it reports. At the
   payment whose bill first lists the owed request, test_forced.c's provider sells every member but the one
   switched in (tf_sale_act), so the member owing the request is relocated before its reply is written. The
   member's report is reached only if it returns within the session.
   `sale`: the arm against the example owes the request and the page's own run is busy. `saleown`: the page's own
   run owes it and the arm against the example is busy. `salectl`: `sale`'s shape with no act, the control.
   `salefloor`: `sale`'s shape, sold with the allocator placed at its floor (tf_floor_place), so the sale's own
   allocations are refused unless the sale funds them. `saleasync`: `sale`'s shape where the owing arm first
   settles a promise made before the fork, so its delta holds that promise's settlement and its resolving
   function's latch when it is sold, and its report reads the settled value. `salekeep`, `salector`,
   `salecatch`, `salepexec` and `saleasyncfn` are described at their constants. Ten of test_forced.c's exploring
   documents (TF_DOCS); each ends with TF_CANVAS_INK, which test_forced.c defines before including this file. */
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

static const char HTML_SALEFLOOR[] =
    "<!doctype html><html><body>"
    "<script>"
    "var slFloor = location.hash === '#owe';"
    "if (slFloor) {"
    "  fetch('/api/sl-floor-owed').then(function (r) { return r.text(); })"
    "    .then(function (t) { fetch('/api/sl-floor-done?b=' + t.length); });"
    "} else {"
    TF_SALE_BUSY
    "  fetch('/api/sl-floor-busy?n=' + slN);"
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

static const char HTML_SALEASYNC[] =
    "<!doctype html><html><body>"
    "<script>"
    "var slaRes, slaP = new Promise(function (r) { slaRes = r; });"
    "var slAsync = location.hash === '#owe';"
    "if (slAsync) {"
    "  slaRes(7);"
    "  fetch('/api/sl-async-owed').then(function (r) { return r.text(); })"
    "    .then(function (t) { slaP.then(function (v) { fetch('/api/sl-async-done?b=' + t.length + '&v=' + v); }); });"
    "} else {"
    TF_SALE_BUSY
    "  fetch('/api/sl-async-busy?n=' + slN);"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

/* `sale`'s shape where, at the act, a holder outside the sold member (test_forced.c's g_tf_keep) names the
   member's owed record, so its restore gives that record back rather than a copy of it. */
static const char HTML_SALEKEEP[] =
    "<!doctype html><html><body>"
    "<script>"
    "var slKeep = location.hash === '#owe';"
    "if (slKeep) {"
    "  fetch('/api/sl-keep-owed').then(function (r) { return r.text(); })"
    "    .then(function (t) { fetch('/api/sl-keep-done?b=' + t.length); });"
    "} else {"
    TF_SALE_BUSY
    "  fetch('/api/sl-keep-busy?n=' + slN);"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

/* `sale`'s shape where the busy arm's loop runs in a constructor's body, so the member sold beside the owing
   one is parked in a heap frame whose continuation is the construct's record (CONT_CONSTRUCT). */
static const char HTML_SALECTOR[] =
    "<!doctype html><html><body>"
    "<script>"
    "function SlCtor() {" TF_SALE_BUSY " this.n = slN; }"
    "var slCtorOwe = location.hash === '#owe';"
    "if (slCtorOwe) {"
    "  fetch('/api/sl-ctor-owed').then(function (r) { return r.text(); })"
    "    .then(function (t) { fetch('/api/sl-ctor-done?b=' + t.length); });"
    "} else {"
    "  var slC = new SlCtor();"
    "  fetch('/api/sl-ctor-busy?n=' + slC.n);"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

/* `sale`'s shape where the busy arm's loop runs inside a `try` in a function over its own locals, so the member
   sold beside the owing one holds the catch offset OP_catch pushed (JS_TAG_CATCH_OFFSET) among its frame's live
   values, and no global lookup the loop makes parks it on a property machine instead. */
static const char HTML_SALECATCH[] =
    "<!doctype html><html><body>"
    "<script>"
    "var slCatchOwe = location.hash === '#owe';"
    "if (slCatchOwe) {"
    "  fetch('/api/sl-catch-owed').then(function (r) { return r.text(); })"
    "    .then(function (t) { fetch('/api/sl-catch-done?b=' + t.length); });"
    "} else {"
    "  (function () { var n = 0;"
    "    try { for (var i = 0; i < 200000; i++) n += i & 1; } catch (e) { n = -1; }"
    "    fetch('/api/sl-catch-busy?n=' + n); })();"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

/* `sale`'s shape where the busy arm's loop runs in a promise executor reached through two Reflect.construct calls,
   the spelling of a transpiled class extending Promise (Babel's _wrapNativeSuper wrapper). The member sold beside
   the owing one stands in the executor's frame (CONT_PROMISE_EXEC, requested by the inner Reflect.construct
   machine) above the wrapper's body (CONT_CONSTRUCT, requested by the outer one). The wrapper builds its list
   rather than passing `arguments`, whose class has no writer and is not in grafana's shape. */
static const char HTML_SALEPEXEC[] =
    "<!doctype html><html><body>"
    "<script>"
    "function SlWrap(ex) { return Reflect.construct(Promise, [ex], new.target); }"
    "SlWrap.prototype = Object.create(Promise.prototype);"
    "var slPexecOwe = location.hash === '#owe';"
    "if (slPexecOwe) {"
    "  fetch('/api/sl-pexec-owed').then(function (r) { return r.text(); })"
    "    .then(function (t) { fetch('/api/sl-pexec-done?b=' + t.length); });"
    "} else {"
    "  var slP = Reflect.construct(SlWrap, [function (res) { var n = 0;"
    "    for (var i = 0; i < 200000; i++) n += i & 1; res(n); }], SlWrap);"
    "  slP.then(function (n) { fetch('/api/sl-pexec-busy?n=' + n); });"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

/* `sale`'s shape where the busy arm's loop runs in an async function's body before its first await, so the member
   sold beside the owing one stands in an async heap frame (TrampFrame.async_data) its script called. */
static const char HTML_SALEASYNCFN[] =
    "<!doctype html><html><body>"
    "<script>"
    "var slAfOwe = location.hash === '#owe';"
    "if (slAfOwe) {"
    "  fetch('/api/sl-asyncfn-owed').then(function (r) { return r.text(); })"
    "    .then(function (t) { fetch('/api/sl-asyncfn-done?b=' + t.length); });"
    "} else {"
    "  (async function () { var n = 0;"
    "    for (var i = 0; i < 200000; i++) n += i & 1;"
    "    fetch('/api/sl-asyncfn-busy?n=' + n); })();"
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
