/* THE GEOMETRY INTERFACES DOCUMENT (`dommatrix`): Geometry Interfaces §2 "The DOMPoint interfaces", §5 "The
   DOMQuad interface" and §6 "The DOMMatrix interfaces" (drafts.csswg.org/geometry, fetched), each member read
   back the way WPT css/geometry's tests at bf4714d assert it, with Chrome 153.0.8010.12's answers as the
   expectations. A value is reported with every character outside `A-Za-z0-9.-` written as `_`.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_DOMMATRIX_H
#define ENGINE_HOST_FIXTURE_DOMMATRIX_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_DOMMATRIX[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<div id=t style=\"transform:translate(10px, 20px)\"></div>"
    "<script>"
    "function dmE(f) { try { f(); return 'none'; } catch (e) { return e.name; } }"
    "function dmR(v) { return Math.round(v * 1000) / 1000; }"
    "var dmEl = document.getElementById('t');"
    "var dmQ = {"
    "  ty: [typeof DOMMatrix, typeof DOMMatrixReadOnly, typeof DOMPoint, typeof DOMPointReadOnly, typeof DOMQuad,"
    "       WebKitCSSMatrix === DOMMatrix].join('_'),"
    "  st: (function () { var m = new DOMMatrix('scale(2) translateX(5px) translateY(5px)'); return [m.a, m.e, m.f, m.is2D].join('_'); })(),"
    "  s2: new DOMMatrix([2, 0, 0, 2, 10, 10]).toString(),"
    "  s3: new DOMMatrix().scale3dSelf(2).toString(),"
    "  ch: new DOMMatrix().translateSelf(20, 20).scaleSelf(2).translateSelf(-20, -20).toString(),"
    "  iv: new DOMMatrix([2, 0, 0, 2, 10, 10]).inverse().toString(),"
    "  ni: (function () { var m = new DOMMatrix([0, 0, 0, 0, 0, 0]).inverse(); return [m.a, m.is2D].join('_'); })(),"
    "  tp: (function () { var p = new DOMMatrix([2, 0, 0, 2, 10, 10]).transformPoint({x: 5, y: 4}); return [p.x, p.y, p.z, p.w].join('_'); })(),"
    "  mt: (function () { var p = new DOMPoint(5, 4).matrixTransform(new DOMMatrix([2, 0, 0, 2, 10, 10])); return [p.x, p.y, p instanceof DOMPoint].join('_'); })(),"
    "  id: [new DOMMatrix().isIdentity, new DOMMatrix('none').isIdentity, new DOMMatrix('').is2D, new DOMMatrix([1, 0, 0, 1, 0, 1]).isIdentity].join('_'),"
    "  ex: [dmE(function () { new DOMMatrix('translateX(5em)'); }), dmE(function () { new DOMMatrix('translateX(5%)'); }),"
    "       dmE(function () { new DOMMatrix('rotate(5)'); }), dmE(function () { new DOMMatrix([1, 2, 3]); }),"
    "       dmE(function () { DOMMatrix.fromMatrix({a: 1, m11: 2}); }), dmE(function () { DOMMatrix.fromMatrix({is2D: true, m33: 2}); }),"
    "       dmE(function () { String(new DOMMatrix([NaN, 0, 0, 1, 0, 0])); })].join('_'),"
    "  fm: [DOMMatrix.fromMatrix({m13: 1}).is2D, DOMMatrix.fromMatrix({e: 7}).m41, DOMMatrixReadOnly.fromMatrix({}) instanceof DOMMatrix].join('_'),"
    "  mu: new DOMMatrix().translate(10, 0).multiply(new DOMMatrix().scale(2)).toString(),"
    "  pm: new DOMMatrix().translate(10, 0).preMultiplySelf(new DOMMatrix().scale(2)).toString(),"
    "  ro: (function () { var m = new DOMMatrix().rotate(90); return [dmR(m.a), dmR(m.b), dmR(m.c), dmR(m.d)].join('_'); })(),"
    "  r3: (function () { var m = new DOMMatrix().rotate(90, 0, 0); return [dmR(m.m22), dmR(m.m23), dmR(m.m32), m.is2D].join('_'); })(),"
    "  ra: (function () { var m = new DOMMatrix().rotateAxisAngle(0, 0, 1, 90); return [dmR(m.a), dmR(m.b), m.is2D].join('_'); })(),"
    "  rv: (function () { var m = new DOMMatrix().rotateFromVector(0, 1); return [dmR(m.a), dmR(m.b)].join('_'); })(),"
    "  sk: [dmR(new DOMMatrix().skewX(45).c), dmR(new DOMMatrix().skewY(45).b), new DOMMatrix().flipX().a, new DOMMatrix().flipY().d].join('_'),"
    "  sc: [new DOMMatrix().scale(2, 3).toString(), new DOMMatrix().scale(2, 2, 1, 10, 10).toString(), new DOMMatrix().scaleNonUniform(2, 3).d].join('_'),"
    "  qd: (function () { var b = DOMQuad.fromRect({x: 2, y: 0, width: 10, height: 10}).getBounds(); return [b.x, b.y, b.width, b.height].join('_'); })(),"
    "  qb: (function () { var q = new DOMQuad({x: 40, y: 25}, {x: 180, y: 8}, {x: 210, y: 150}, {x: 10, y: 180}); var b = q.getBounds(); return [b.x, b.y, b.width, b.height, q.p1 === q.p1, q.p2.x].join('_'); })(),"
    "  qq: (function () { var q = DOMQuad.fromQuad({p1: {x: 1}, p3: {y: 2}}); return [q.p1.x, q.p3.y, q.p4.w].join('_'); })(),"
    "  js: [JSON.stringify(new DOMPoint(1, 2)), JSON.stringify(new DOMMatrixReadOnly([1, 2, 3, 4, 5, 6])).length].join('_'),"
    "  wk: (function () { var m = new WebKitCSSMatrix(getComputedStyle(dmEl).transform); return [m.m41, m.m42, m.is2D].join('_'); })(),"
    "  fa: [DOMMatrix.fromFloat32Array(new Float32Array([1, 0, 0, 1, 5, 6])).f, new DOMMatrix().toFloat64Array().length,"
    "       new DOMMatrix([1, 2, 3, 4, 5, 6]).toFloat32Array()[4]].join('_'),"
    "  rt: new DOMMatrix(new DOMMatrix([1, 2, 3, 4, 5, 6])).toString(),"
    "  se: (function () { var m = new DOMMatrix(); m.m13 = 1; var n = new DOMMatrix(); n.e = 3; return [m.is2D, n.m41, n.is2D].join('_'); })(),"
    "  sv: new DOMMatrix().setMatrixValue('translate(1px, 2px)').toString(),"
    "  ro2: (function () { var m = new DOMMatrixReadOnly(); return [typeof m.translateSelf, m.translate(1, 2) instanceof DOMMatrix, Object.getPrototypeOf(DOMMatrix) === DOMMatrixReadOnly].join('_'); })(),"
    "  pt: (function () { var p = new DOMPoint(); p.x = 3; var r = new DOMPointReadOnly(1, 2, 3, 4); return [p.x, p.w, r.z, DOMPoint.fromPoint({y: 2}).y, typeof r.matrixTransform].join('_'); })(),"
    "};"
    "var dmS = Object.keys(dmQ).map(function (k) { return k + '=' + String(dmQ[k]).replace(/[^A-Za-z0-9.-]/g, '_'); }).join('&');"
    "fetch('/api/dommatrix?' + dmS);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
