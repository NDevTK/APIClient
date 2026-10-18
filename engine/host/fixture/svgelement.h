/* THE SVG ELEMENT DOCUMENT: SVG 2 §4.4.1 Interface SVGElement on the SVG-namespace elements an inline `<svg>`
   parses into. One of test_forced.c's exploring documents (its TF_DOCS table). Its script reads, for a
   `<circle>` inside the `<svg>`: the interface chain (SVGElement, then Element, and not HTMLElement), §4.4.1's
   `className` as a [SameObject] SVGAnimatedString reflecting `class` in both directions (§4.6.7), the nearest
   `svg` ancestor that `ownerSVGElement` and `viewportElement` both answer (null on the outermost svg), and the
   members SVGElement includes: CSSOM's `style`, and HTML §3.2.2's HTMLOrSVGOrMathMLElement `dataset`,
   `tabIndex` (−1, and 0 on an SVG `a` by §6.6.3) and `focus`. Vue 3's runtime-dom resolves an app's root
   namespace with `e instanceof SVGElement`, unguarded, which is the read `ns` makes. Chrome 155 gives these
   values for the same markup. It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SVGELEMENT_H
#define ENGINE_HOST_FIXTURE_SVGELEMENT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SVGELEMENT[] =
    "<!doctype html><html>"
    "<body>"
    "<div id=host></div>"
    "<svg id=outer width=100 height=100><g><circle id=c class=ring data-k=v r=10></circle></g>"
    "<a id=sa href=#x><text>t</text></a></svg>"
    "<script>var s = document.getElementById('outer'), c = document.getElementById('c');"
    "var cn = c.className, before = cn.baseVal;"
    "cn.baseVal = 'set';"
    "fetch('/api/svgel?inst=' + (c instanceof SVGElement ? 1 : 0)"
    " + '&el=' + (c instanceof Element ? 1 : 0)"
    " + '&html=' + (c instanceof HTMLElement ? 1 : 0)"
    " + '&tag=' + Object.prototype.toString.call(c).slice(8, -1)"
    " + '&cn=' + before"
    " + '&same=' + (c.className === cn ? 1 : 0)"
    " + '&set=' + c.getAttribute('class')"
    " + '&anim=' + cn.animVal"
    " + '&own=' + (c.ownerSVGElement === s ? 1 : 0)"
    " + '&root=' + (s.ownerSVGElement === null ? 1 : 0)"
    " + '&vp=' + (c.viewportElement === s ? 1 : 0)"
    " + '&st=' + typeof c.style"
    " + '&ds=' + c.dataset.k"
    " + '&ti=' + c.tabIndex"
    " + '&ta=' + document.getElementById('sa').tabIndex"
    " + '&fo=' + typeof c.focus"
    " + '&ns=' + (s instanceof SVGElement ? 'svg' : 'none'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
