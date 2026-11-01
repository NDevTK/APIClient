/* THE STRING-COMPILATION CSP DOCUMENT: a policy whose script-src carries no 'unsafe-eval' (and 'report-sample').
   HTML §8.1.6.2 HostEnsureCanCompileStrings runs CSP §4.4.1 EnsureCSPDoesNotBlockStringCompilation, so a direct
   eval, an indirect eval and a Function constructor each throw an EvalError and report a script-src violation
   whose sample is the code's first 40 characters, and HTML §8.7 Timers' step 1.5 returns 0 for a string
   handler. The violations are queued tasks, read after a timer.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CSP_EVAL_H
#define ENGINE_HOST_FIXTURE_CSP_EVAL_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CSP_EVAL[] =
    "<!doctype html><html><head>"
    "<meta http-equiv='Content-Security-Policy' content=\"script-src 'self' 'unsafe-inline' 'report-sample'\">"
    "</head><body>"
    "<script>"
    "var ceV = [], ceOne = 1;"
    "document.addEventListener('securitypolicyviolation', function (e) { ceV.push(e); });"
    "function ceTry(f) { try { f(); return 'ran'; } catch (e) { return e.name; } }"
    "var ceD = ceTry(function () { eval('ceOne'); });"
    "var ceI = ceTry(function () { (0, eval)('ceOne'); });"
    "var ceF = ceTry(function () { new Function('a', 'return a'); });"
    "var ceT = setTimeout('window.ceRan = 1', 0);"
    "fetch('/api/ce-throws?d=' + ceD + '&i=' + ceI + '&f=' + ceF + '&t=' + ceT);"
    "setTimeout(function () {"
    "  var v = ceV[0] || {};"
    "  fetch('/api/ce-viol?n=' + ceV.length + '&dir=' + v.violatedDirective + '&uri=' + v.blockedURI"
    "        + '&smp=' + v.sample + '&r=' + (window.ceRan === 1));"
    "}, 20);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
