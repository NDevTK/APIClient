/* THE STATIC POSITION ASK DOCUMENT: CSS 2.1 §10.3.7's and §10.6.4's static position, read by a used length that
   a resting drive asks. One of test_forced.c's exploring documents (its TF_DOCS table). Two absolutely
   positioned boxes are appended to <html> after <body>, as jQuery 3.7's support tests append theirs, so each
   static position is placed by §9.4.1's stack of <html>'s children. The first has `top` and `bottom` auto: its
   `height` is its 100px child's, and that child's resolved `top: 10%` is 10px of it. The second has `left`
   and `right` auto: its shrink-to-fit `width` is its 40px child's, and its left edge is <html>'s 7px content
   edge. The first box's static top is 70px below <html>'s: 8px of body margin, 30px and 20px blocks, and the
   12px bottom margin that collapses through body's 8px. Chrome 153.0.8010.12 gives every number the rows
   expect for this markup. It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_STATICPOSASK_H
#define ENGINE_HOST_FIXTURE_STATICPOSASK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_STATICPOSASK[] =
    "<!doctype html><html style=\"padding-left:7px\">"
    "<body>"
    "<div style=\"height:30px\"></div><div style=\"height:20px;margin-bottom:12px\"></div>"
    "<script>"
    "var c = document.createElement('div'), d = document.createElement('div');"
    "c.style.cssText = 'position:absolute;left:-11111px;width:60px;margin-top:0;padding:0;border:0';"
    "d.style.cssText = 'position:relative;display:block;margin:auto;border:0;padding:0;width:60%;height:100px;"
    "top:10%';"
    "c.appendChild(d); document.documentElement.appendChild(c);"
    "var w = document.createElement('div'), x = document.createElement('div');"
    "w.style.cssText = 'position:absolute;top:0'; x.style.cssText = 'width:40px;height:5px';"
    "w.appendChild(x); document.documentElement.appendChild(w);"
    "fetch('/api/staticposask?h=' + getComputedStyle(c).height + '&t=' + getComputedStyle(d).top"
    " + '&w=' + getComputedStyle(w).width + '&y=' + c.getBoundingClientRect().top"
    " + '&x=' + w.getBoundingClientRect().left);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
