/* THE CASCADE FORK OVER A RECORD MEMBER: Selectors 4 §6.1's `[att=val]` holding for an attribute written from
   a member of JSON.parse over a storage read, the shape a persisted settings store gives a theme. One of
   test_forced.c's exploring documents (its TF_DOCS table). The member may be any JSON value, an Object among
   them, so a holding `[data-theme="dark"]` determines the attribute's string and not the member: its type set
   admits an Array whose ToString is `dark`. #t is 50px where `dark` holds, 30px where `light` does, 10px
   otherwise, and 90px only in a world where both equalities held. Each world reports #t's width. It ends with
   TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CASCFORKJSON_H
#define ENGINE_HOST_FIXTURE_CASCFORKJSON_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CASCFORKJSON[] =
    "<!doctype html><html>"
    "<head><style>"
    "#t{width:10px;height:1px}"
    ":root[data-theme=\"dark\"] #t{width:50px}"
    ":root[data-theme=\"light\"] #t{width:30px}"
    ":root[data-theme=\"dark\"][data-theme=\"light\"] #t{width:90px}"
    "</style></head>"
    "<body style=\"margin:0\"><div id=t></div>"
    "<script>"
    "var st={};"
    "try{st=JSON.parse(localStorage.getItem('settings'))||{};}catch(e){}"
    "document.documentElement.setAttribute('data-theme',st.theme);"
    "fetch('/api/cfmember?w='+document.getElementById('t').clientWidth);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
