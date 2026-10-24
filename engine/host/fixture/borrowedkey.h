/* THE BORROWED-KEY DOCUMENT: an object spread whose source property's value is its own key's string, so
   ECMAScript §7.3.25 CopyDataProperties ( target, source, excludedItems ) defines a property whose key is
   borrowed from the walk's own-key snapshot and whose value holds a reference on that key's atom too. The
   copy's first key gives it a shape no other object has, so the define adds the key to a new shape as well.
   Both objects are a function's locals, so the copied value is the concrete string itself rather than an
   unknown a global's property read would mint. The define's key ledger admits those two references; the fetch
   after the spread reports the copied value.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_BORROWEDKEY_H
#define ENGINE_HOST_FIXTURE_BORROWEDKEY_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_BORROWEDKEY[] =
    "<!doctype html><html><body>"
    "<script>"
    "(function () {"
    "  var src = { shared: 'shared' };"
    "  var copy = { bkFirst: 1, ...src };"
    "  fetch('/api/borrowedkey?v=' + copy.shared);"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
