/* THE KEY-DOMAIN WALK DOCUMENT: 2,000 `<span>` elements carrying no id and no name, and 100 reads of a Document
   and a WindowProxy keyed by an unknown String whose leading bytes the page wrote (`"zq" + Math.random()`). No
   supported property name lies in the domain, so each read misses; HTML §3.1.7's and §7.2.2.3's names are id and
   name values, so the miss is decided over the document's name index without visiting an element. The run's
   `twDomWalkInput` then stays far below the 400,000 a document walk per read would charge.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_KEYDOMAINWALK_H
#define ENGINE_HOST_FIXTURE_KEYDOMAINWALK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define KDW_10 "<span></span><span></span><span></span><span></span><span></span>" \
               "<span></span><span></span><span></span><span></span><span></span>"
#define KDW_100 KDW_10 KDW_10 KDW_10 KDW_10 KDW_10 KDW_10 KDW_10 KDW_10 KDW_10 KDW_10
#define KDW_1000 KDW_100 KDW_100 KDW_100 KDW_100 KDW_100 KDW_100 KDW_100 KDW_100 KDW_100 KDW_100

static const char HTML_KEYDOMAINWALK[] =
    "<!doctype html><html><body><div>" KDW_1000 KDW_1000 "</div>"
    "<script>"
    "var kdwKey = 'zq' + Math.random(), kdwHits = 0, kdwI;"
    "for (kdwI = 0; kdwI < 100; kdwI++) { if (document[kdwKey]) kdwHits++; if (window[kdwKey]) kdwHits++; }"
    "fetch('/api/kdw-done?h=' + kdwHits);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
