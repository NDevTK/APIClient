/* THE CASCADE FORK OVER A DERIVATION: Selectors 4 §6.1's `[att=val]` holding for an attribute written from a
   concatenation over a storage read. One of test_forced.c's exploring documents (its TF_DOCS table). The
   concatenation owns no source of its own, so a holding `[data-mode="m-wide"]` determines the attribute's
   string under the derived value's identity and pins nothing on the storage read. #u is 20px where `m-wide`
   holds, 7px where `m-slim` does, 5px otherwise, and 90px only in a world where both equalities held. Each
   world reports #u's width. It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CASCFORKDERIVED_H
#define ENGINE_HOST_FIXTURE_CASCFORKDERIVED_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CASCFORKDERIVED[] =
    "<!doctype html><html>"
    "<head><style>"
    "#u{width:5px;height:1px}"
    "body[data-mode=\"m-wide\"] #u{width:20px}"
    "body[data-mode=\"m-slim\"] #u{width:7px}"
    "body[data-mode=\"m-wide\"][data-mode=\"m-slim\"] #u{width:90px}"
    "</style></head>"
    "<body style=\"margin:0\"><div id=u></div>"
    "<script>"
    "document.body.setAttribute('data-mode','m-'+localStorage.getItem('mode'));"
    "fetch('/api/cascforkderived?w='+document.getElementById('u').clientWidth);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
