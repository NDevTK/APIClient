/* THE TYPED-ARRAY INDEX DOCUMENT: %TypedArray%.prototype.fill (ECMAScript §23.2.3.9) and copyWithin (§23.2.3.6) over
   arguments the fragment supplies. A fill from a start the fragment chooses (6 where it chooses none) witnesses the
   arm where element 5 holds the filled value; a copyWithin from a start the fragment chooses (0 where it chooses
   none) witnesses the arm where element 0 holds element 3's value; a fill whose value the fragment chooses (0
   where it chooses none) witnesses the arm where element 2 holds 9. Over `[5, 0, 5, 0]`, indexOf (§23.2.3.17),
   lastIndexOf (§23.2.3.20) and includes (§23.2.3.16) from the fragment's fromIndex witness the arms answering 2,
   0 and false. A set (§23.2.3.26) of an array-like and of a typed array at the fragment's offset (0 where it
   chooses none) witnesses the arms where element 2 and element 3 hold 9, and a two-element set into a
   two-element view witnesses an arm throwing RangeError. No arm the fragment leaves at its default reaches any of
   these. Each witness fetch carries a
   constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_TAINDEX_H
#define ENGINE_HOST_FIXTURE_TAINDEX_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_TAINDEX[] =
    "<!doctype html><html><body>"
    "<script>"
    "var taiN = +location.hash.slice(1);"
    "var taiA = new Uint8Array(8); taiA.fill(7, taiN || 6);"
    "if (taiA[5] === 7) fetch('/api/tai-fill5');"
    "var taiB = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]); taiB.copyWithin(0, taiN || 0);"
    "if (taiB[0] === 4) fetch('/api/tai-copy4');"
    "var taiC = new Uint8Array(4); taiC.fill(taiN || 0);"
    "if (taiC[2] === 9) fetch('/api/tai-fillv9');"
    "var taiD = new Uint8Array([5, 0, 5, 0]);"
    "if (taiD.indexOf(5, taiN || 0) === 2) fetch('/api/tai-index2');"
    "if (taiD.lastIndexOf(5, taiN || 3) === 0) fetch('/api/tai-last0');"
    "if (!taiD.includes(5, taiN || 0)) fetch('/api/tai-incl-none');"
    "var taiE = new Uint8Array(4); taiE.set([9], taiN || 0);"
    "if (taiE[2] === 9) fetch('/api/tai-set2');"
    "var taiF = new Uint8Array(4); taiF.set(new Uint8Array([9]), taiN || 0);"
    "if (taiF[3] === 9) fetch('/api/tai-setta3');"
    "try { new Uint8Array(2).set([1, 2], taiN || 0); }"
    "catch (e) { if (e instanceof RangeError) fetch('/api/tai-set-range'); }"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
