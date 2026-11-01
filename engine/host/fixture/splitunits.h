/* THE SPLIT-UNITS DOCUMENT: jQuery's expando (`"jQuery" + ("3.7.1" + Math.random()).replace(/\D/g, "")`) split by
   the empty separator, as Sizzle's sortStable test splits it. Each element is one code unit: a unit of the page's
   own "jQuery" is that letter in every world, so comparing two of them, or one with a literal, forks nothing
   (su-a1, su-a2); a unit of the replace's digits is an unknown whose code units are digits, so it equals no
   letter (su-a3, su-a4, and the sort comparator's su-a6). Two digit units, or a digit unit and a digit literal,
   may be equal and keep both arms (su-a5/su-b5, su-a7/su-b7). One of test_forced.c's exploring documents
   (TF_DOCS); it ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SPLITUNITS_H
#define ENGINE_HOST_FIXTURE_SPLITUNITS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SPLITUNITS[] =
    "<!doctype html><html><body>"
    "<script>"
    "var suKey = 'jQuery' + ('3.7.1' + Math.random()).replace(/\\D/g, '');"
    "var suUnits = suKey.split('');"
    "if (suUnits[0] === 'j') fetch('/api/su-b1'); else fetch('/api/su-a1');"
    "if (suUnits[1] === suUnits[5]) fetch('/api/su-a2'); else fetch('/api/su-b2');"
    "if (suUnits[7] === 'j') fetch('/api/su-a3'); else fetch('/api/su-b3');"
    "if (suUnits[0] === suUnits[8]) fetch('/api/su-a4'); else fetch('/api/su-b4');"
    "if (suUnits[7] === suUnits[8]) fetch('/api/su-a5'); else fetch('/api/su-b5');"
    "var suLetterDup = false;"
    "suKey.split('').sort(function (e, t) { if (e === t && e === 'Q') suLetterDup = true; return 0; });"
    "if (suLetterDup) fetch('/api/su-a6'); else fetch('/api/su-b6');"
    "if (suUnits[7] === '7') fetch('/api/su-a7'); else fetch('/api/su-b7');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
