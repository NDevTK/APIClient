/* THE SCRIPT-CREATED PARSER DOCUMENT: HTML §8.4.1's document.open() stream written across a fork.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_DWFORK_H
#define ENGINE_HOST_FIXTURE_DWFORK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_DWFORK[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    /* §8.4.1 leaves the frame's stream open across the branch, and `state.flag` is absent app state, so the
       branch forks with one lxb_html_parser_t standing mid-document. Each arm writes into its own copy of the
       parser: the true arm's document is `<div id="r"><b>A</b></div>` and the false arm's
       `<div id="r"><i>B</i></div>`, the serializations Chrome gives the same writes. */
    "var dwf = document.createElement('iframe'); document.body.appendChild(dwf);"
    "var dwd = dwf.contentDocument; dwd.open(); dwd.write('<div id=r>');"
    "if (state.flag) dwd.write('<b>A</b>'); else dwd.write('<i>B</i>');"
    "dwd.write('</div>'); dwd.close();"
    "var dwh = dwd.body.innerHTML;"
    "fetch('/api/dwfork?v=' + (dwh === '<div id=\"r\"><b>A</b></div>' ? 'DWA'"
    " : dwh === '<div id=\"r\"><i>B</i></div>' ? 'DWB' : 'DWBAD:' + encodeURIComponent(dwh)));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
