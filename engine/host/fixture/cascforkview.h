/* THE CASCADE FORK VIEW DOCUMENT: Selectors 4 §17.3 answering UNKNOWN inside the cascade of an element whose
   `scrollWidth` is read. One of test_forced.c's exploring documents (its TF_DOCS table). The root's
   `data-mode` is written from a storage read the page never stored, and `[data-mode="wide"]` widens #w from
   10px to 40px. `scrollWidth` is a step machine whose box tests read #w's author matches under a resting drive,
   so the predicate forks there: the world where it holds pins the value to `wide`, and the other keeps the
   storage read's example. Each world reports `scrollWidth-value`. It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CASCFORKVIEW_H
#define ENGINE_HOST_FIXTURE_CASCFORKVIEW_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CASCFORKVIEW[] =
    "<!doctype html><html>"
    "<head><style>"
    "#w{width:10px;height:1px;overflow:hidden}"
    ":root[data-mode=\"wide\"] #w{width:40px}"
    "</style></head>"
    "<body style=\"margin:0\"><div id=w></div>"
    "<script>"
    "var md=localStorage.getItem('mode');"
    "document.documentElement.setAttribute('data-mode',md);"
    "fetch('/api/viewfork?r='+document.getElementById('w').scrollWidth+'-'+md);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
