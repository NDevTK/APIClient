/* THE CASCADE FORK DOCUMENT: Selectors 4 §17.3 answering UNKNOWN inside the cascade, kept as two worlds. One of
   test_forced.c's exploring documents (its TF_DOCS table). The root's `data-theme` is written from a storage
   read the page never stored, so its value is unknown, and two rules test it: `[data-theme="dark"]` sizes #t to
   50px and `[data-theme="light"]` to 30px, over a 10px default. `clientWidth` is a step machine that owns a
   layout job, so its cascade forks on each predicate. A holding `=` pins the value, so the dark world never
   asks the light test (the pinned bytes answer it) and the page's own read of the value is the pinned string.
   #u's rules are tested after #t's answers: `[data-theme="light"]` again, which replays the answer the flow
   already holds, and `[data-theme^="d"]`, which the pinned bytes decide and the unpinned world forks. Each
   world reports `width of #t - width of #u - the value`, so a world where both equalities held, or where a
   replayed answer was forked again, is a fifth value. It ends with TF_CANVAS_INK, which test_forced.c defines
   before including it. */
#ifndef ENGINE_HOST_FIXTURE_CASCFORK_H
#define ENGINE_HOST_FIXTURE_CASCFORK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CASCFORK[] =
    "<!doctype html><html>"
    "<head><style>"
    "#t{width:10px;height:1px}"
    ":root[data-theme=\"dark\"] #t{width:50px}"
    ":root[data-theme=\"light\"] #t{width:30px}"
    "#u{width:5px;height:1px}"
    ":root[data-theme=\"light\"] #u{width:7px}"
    ":root[data-theme^=\"d\"] #u{width:20px}"
    "</style></head>"
    "<body style=\"margin:0\"><div id=t></div><div id=u></div>"
    "<script>"
    "var th=localStorage.getItem('theme');"
    "document.documentElement.setAttribute('data-theme',th);"
    "var tw=document.getElementById('t').clientWidth;"
    "var uw=document.getElementById('u').clientWidth;"
    "fetch('/api/cascfork?r='+tw+'-'+uw+'-'+th);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
