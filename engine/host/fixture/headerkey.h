/* THE HEADER-KEY DOCUMENT: Fetch §5.1 fill a Headers object from a record whose one own key is unknown, an
   ordinary object written under `location.hash.slice(1)`, so its @@iterator is Object.prototype's (none) and the
   fill takes the record arm, whose enumeration hands the unknown back as the key. Web IDL §3.2.23 converts the
   key to ByteString (§3.2.11 step 2 throws for an element above 255) and §5.1's append validates it as a header
   name (validate a header step 1 throws a TypeError), so `new Headers(rec)` either builds one header
   (hk-built?n=1) or throws a TypeError (hk-threw), and `fetch` with the same record either sends (hk-sent) or
   rejects (hk-refused). One of test_forced.c's exploring documents (its TF_DOCS table). It ends with
   TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_HEADERKEY_H
#define ENGINE_HOST_FIXTURE_HEADERKEY_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_HEADERKEY[] =
    "<!doctype html><html><body>"
    "<script>"
    "var rec = {};"
    "rec[location.hash.slice(1)] = 'v';"
    "try {"
    "  var h = new Headers(rec);"
    "  fetch('/api/hk-built?n=' + Array.from(h.keys()).length);"
    "} catch (e) {"
    "  fetch('/api/hk-threw?e=' + e.name);"
    "}"
    "fetch('/api/hk-sent', { headers: rec }).then(function () {}, function (e) {"
    "  fetch('/api/hk-refused?e=' + e.name);"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
