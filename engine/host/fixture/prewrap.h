/* The preserved-space wrapping document (`prewrap`): css-text-3 §3's `pre-wrap` and `break-spaces` in a box five
   monospace characters wide. Each case reports its line count, its max-content and min-content widths (an
   inline-block, and one in a zero-width box), the min-content box's line count, and where each character's
   rectangle is: `<x>x<line>w<width>` in character cells. Under `pre-wrap` the spaces and tabs that end a line
   hang (they keep their advance, past the box's edge, and are left out of the line's fit and of min-content);
   under `break-spaces` they take up space and a line can break after each one.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_PREWRAP_H
#define ENGINE_HOST_FIXTURE_PREWRAP_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_PREWRAP[] =
    "<!doctype html><html><body style=\"margin:0;font:16px/20px monospace\">"
    "<span id=pwC>a</span><div id=pwHost></div>"
    "<script>"
    "var pwCw = document.getElementById('pwC').getBoundingClientRect().width;"
    "var pwHost = document.getElementById('pwHost');"
    "function pwN(v) { return Math.round(v / pwCw * 10) / 10; }"
    "function pwBox(css, txt, parent) {"
    "  var e = document.createElement('div'); e.style.cssText = css; e.textContent = txt;"
    "  parent.appendChild(e); return e;"
    "}"
    "function pwCase(ws, txt) {"
    "  var d = pwBox('width:5ch;white-space:' + ws, txt, pwHost);"
    "  var mx = pwBox('display:inline-block;white-space:' + ws, txt, pwHost);"
    "  var z = pwBox('width:0', '', pwHost);"
    "  var mn = pwBox('display:inline-block;white-space:' + ws, txt, z);"
    "  var t = d.firstChild, o = d.getBoundingClientRect(), r = document.createRange(), s = [], i, c;"
    "  for (i = 0; i < txt.length; i++) {"
    "    if (txt.charAt(i) === '\\n') continue;"
    "    r.setStart(t, i); r.setEnd(t, i + 1); c = r.getClientRects()[0];"
    "    s.push(pwN(c.left - o.left) + 'x' + Math.round((c.top - o.top) / 20) + 'w' + pwN(c.width));"
    "  }"
    "  return 'h' + Math.round(o.height / 20) + 'mx' + pwN(mx.getBoundingClientRect().width) +"
    "         'mn' + pwN(mn.getBoundingClientRect().width) + 'mh' + Math.round(mn.getBoundingClientRect().height / 20) +"
    "         '_' + s.join('_');"
    "}"
    "function pwAll(ws, cases) {"
    "  var q = [], k;"
    "  for (k in cases) q.push(k + '=' + pwCase(ws, cases[k]));"
    "  return q.join('&');"
    "}"
    "fetch('/api/prewrap?' + pwAll('pre-wrap', { a: 'aaa  bbb', b: 'aaaaa   bbb', d: '  aa', e: 'aaaa  \\nb',"
    "      f: 'aa   bb  ', h: '       ', k: 'a b c d e f', m: 'aaaa\\tb' }));"
    "fetch('/api/breakspaces?' + pwAll('break-spaces', { b: 'aaaaa   bbb', c: 'aaa      bbb', e: 'aaaa  \\nb',"
    "      h: '       ', k: 'a b c d e f', n: 'aa \\tbb' }));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
