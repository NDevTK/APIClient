/* THE REQUEST-METHOD DOCUMENT: Fetch §5.4's Request constructor and fetch() given a RequestInit `method` the
   page computed, over which step 25 ("If method is not a method or method is a forbidden method, then throw a
   TypeError") and the steps that read the method after it (32.1's CORS-safelisted test under "no-cors", 35's
   GET/HEAD-with-body test) decide.
   - `navigator.language` is an environment unknown with an example: the method is built from it, and the
     arm that returned carries the normalized example, which the request is sent with.
   - `location.hash.slice(1)` is an unknown with no example: the request is recorded with the method's shape.
   - A forbidden method throws step 25's TypeError, and `post` is normalized to `POST`.
   Each statement catches, so every arm reports itself with a witness fetch of a constant address.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_REQMETHOD_H
#define ENGINE_HOST_FIXTURE_REQMETHOD_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_REQMETHOD[] =
    "<!doctype html><html><body>"
    "<script>"
    "try {"
    "  var rqmEx = new Request('/api/rqm-exreq', { method: navigator.language });"
    "  fetch('/api/rqm-ex-built');"
    "  fetch(rqmEx).then(function () { fetch('/api/rqm-ex-replied'); });"
    "} catch (e) { if (e instanceof TypeError) fetch('/api/rqm-ex-threw'); }"
    "</script>"
    "<script>"
    "try {"
    "  var rqmUn = new Request('/api/rqm-unreq', { method: location.hash.slice(1) });"
    "  fetch('/api/rqm-un-built');"
    "  fetch(rqmUn);"
    "} catch (e) { if (e instanceof TypeError) fetch('/api/rqm-un-threw'); }"
    "</script>"
    "<script>"
    "try {"
    "  new Request('/api/rqm-forbid', { method: 'TRACE' });"
    "  fetch('/api/rqm-forbid-built');"
    "} catch (e) { if (e instanceof TypeError) fetch('/api/rqm-forbid-threw'); }"
    "</script>"
    "<script>"
    "fetch(new Request('/api/rqm-post', { method: 'post', body: 'a' }));"
    "</script>"
    "<script>"
    "fetch('/api/rqm-fetchreq', { method: location.hash.slice(1) })"
    "  .catch(function (e) { if (e instanceof TypeError) fetch('/api/rqm-fetch-rejected'); });"
    "</script>"
    "<script>"
    "try {"
    "  new Request('/api/rqm-body', { method: location.hash.slice(1), body: 'x' });"
    "  fetch('/api/rqm-body-built');"
    "} catch (e) { if (e instanceof TypeError) fetch('/api/rqm-body-threw'); }"
    "</script>"
    "<script>"
    "try {"
    "  new Request('/api/rqm-nc', { mode: 'no-cors', method: location.hash.slice(1) });"
    "  fetch('/api/rqm-nc-built');"
    "} catch (e) { if (e instanceof TypeError) fetch('/api/rqm-nc-threw'); }"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
