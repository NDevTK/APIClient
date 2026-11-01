/* THE TRUSTED-TYPES STRING-COMPILATION DOCUMENT: `require-trusted-types-for 'script'` and a default policy whose
   createScript counts its calls and records its sink. CSP §4.4.1 step 2 runs Trusted Types §3.5 get trusted type
   compliant string once per compilation, so each count is 1: a direct and an indirect eval whose parse rests
   inside ten regular expression literals several work quanta long in all, each under V8's literal size limit
   (the rest re-executes the eval over the held, checked source), a direct eval whose createScript forks on the fragment after counting (both arms carry the
   one check), a Function constructor ("Function"), and a string timer ("Window setTimeout").
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_TT_EVAL_H
#define ENGINE_HOST_FIXTURE_TT_EVAL_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_TT_EVAL[] =
    "<!doctype html><html><head>"
    "<meta http-equiv='Content-Security-Policy' content=\"require-trusted-types-for 'script'\">"
    "</head><body>"
    "<script>"
    "var teN = 0, teSink = 'none', teFork = false, teArm = 'o';"
    "trustedTypes.createPolicy('default', { createScript: function (s, type, sink) {"
    "  teN++; teSink = type + ':' + sink.split(' ').join('_');"
    "  if (teFork && location.hash === '#x') teArm = 'x';"
    "  return s;"
    "} });"
    "var teLong = '', teI;"
    "for (teI = 0; teI < 10; teI++) teLong += 'var teR' + teI + ' = /' + 'a'.repeat(30000) + 'b/;';"
    "teLong += 'teR9.test(\"' + 'a'.repeat(30000) + 'b\") && !teR0.test(\"' + 'a'.repeat(30000) + '\")';"
    "(function () {"
    "  var n0 = teN, m = eval(teLong);"
    "  fetch('/api/te-rest?m=' + m + '&n=' + (teN - n0) + '&s=' + teSink);"
    "})();"
    "(function () {"
    "  var n0 = teN, m = (0, eval)(teLong);"
    "  fetch('/api/te-ind?m=' + m + '&n=' + (teN - n0) + '&s=' + teSink);"
    "})();"
    "(function () {"
    "  var n0 = teN, m;"
    "  teFork = true; m = eval('40 + 2'); teFork = false;"
    "  fetch('/api/te-fork?m=' + m + '&n=' + (teN - n0) + '&a=' + teArm);"
    "})();"
    "(function () {"
    "  var n0 = teN, f = new Function('a', 'return a + 1');"
    "  fetch('/api/te-fn?m=' + f(1) + '&n=' + (teN - n0) + '&s=' + teSink);"
    "})();"
    "(function () {"
    "  var n0 = teN, id = setTimeout('window.teRan = 1', 0);"
    "  fetch('/api/te-timer?t=' + (id > 0) + '&n=' + (teN - n0) + '&s=' + teSink);"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
