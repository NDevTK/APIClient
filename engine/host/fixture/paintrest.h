/* THE PAINT REST DOCUMENT. One of test_forced.c's exploring documents (its TF_DOCS table). A hundred absolutely
   positioned 20px boxes, each its own entry in CSS 2.1 §E.2 "Painting order"'s step 8 or 9, plus the step 4 and
   step 7 offers of the flow around them, make about two hundred offers: more than update the rendering step 22
   paints in one activation (solver/rest_unit.h's REST_UNIT_PAINT_OFFERS), so its paint rests between offers at
   least once (`_layoutPaintRests`). The page reads no layout from script, so no other step machine rests on its
   behalf. Its one row is that count; it states no geometry, so no browser value is asserted. It ends with
   TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_PAINTREST_H
#define ENGINE_HOST_FIXTURE_PAINTREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define PR_BOX(x, y, z) "<div style=\"position:absolute;left:" x "px;top:" y "px;width:20px;height:20px;" \
                        "background:#0000ff;z-index:" z "\"></div>"
#define PR_ROW(y) PR_BOX("0", y, "0") PR_BOX("20", y, "1") PR_BOX("40", y, "2") PR_BOX("60", y, "3")   \
                  PR_BOX("80", y, "4") PR_BOX("100", y, "0") PR_BOX("120", y, "1") PR_BOX("140", y, "2") \
                  PR_BOX("160", y, "3") PR_BOX("180", y, "4")

static const char HTML_PAINTREST[] =
    "<!doctype html><html>"
    "<body>"
    PR_ROW("0") PR_ROW("20") PR_ROW("40") PR_ROW("60") PR_ROW("80")
    PR_ROW("100") PR_ROW("120") PR_ROW("140") PR_ROW("160") PR_ROW("180")
    "<div style=\"margin-top:220px;width:100px;height:50px\"></div>"
    "<script>fetch('/api/paintrest');</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef PR_ROW
#undef PR_BOX

#endif
