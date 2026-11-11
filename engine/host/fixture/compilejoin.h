/* The compile-join documents: a classic script writes the decoded fragment to innerHTML, so each @S candidate for
   that sink founds a family of its own beside the page's, and the program that follows is longer than many quanta
   of compile. Every family that reaches the long row pays the compile's work whoever ran its stints, so the
   families take the thread in turn and the later ones pay what the first ran. `compilejoin` holds a root module there;
   `compilejoinclassic` a classic script. The long body opens with a regular expression literal, a tagged template
   and a class with computed keys, whose values its request carries. test_forced.c's exploring documents (its
   TF_DOCS table); each ends with TF_CANVAS_INK, which test_forced.c defines before including this. */
#ifndef ENGINE_HOST_FIXTURE_COMPILEJOIN_H
#define ENGINE_HOST_FIXTURE_COMPILEJOIN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif
#ifndef MS_MODULE_OPEN
#error "fixture/compilejoin.h is included before fixture/modshare.h, whose module opening tag its rows measure from"
#endif

#define CJ_SINK                                                                                       \
    "<div id='cjo'></div>"                                                                            \
    "<script>document.getElementById('cjo').innerHTML = decodeURIComponent(location.hash.slice(1));</script>"
#define CJ_X16(s) s s s s s s s s s s s s s s s s
#define CJ_PAD CJ_X16(CJ_X16(CJ_X16("cjN = (cjN * 31 + 7) % 1000003;\n" "cjN = (cjN * 31 + 7) % 1000003;\n"       \
                                    "cjN = (cjN * 31 + 7) % 1000003;\n" "cjN = (cjN * 31 + 7) % 1000003;\n")))
#define CJ_BODY(path)                                                                                 \
    "const cjRx = /a(b+)c/g;\n"                                                                       \
    "const cjTag = (s, ...v) => s.raw.join('-') + v.length;\n"                                        \
    "const cjTpl = cjTag`x${1}y${2}z`;\n"                                                             \
    "const cjKey = 'k' + 1;\n"                                                                        \
    "class CjC { [cjKey]() { return 7; } static [cjKey + 's'] = 3; }\n"                               \
    "let cjN = 1;\n"                                                                                  \
    CJ_PAD                                                                                            \
    "fetch('" path "?rx=' + cjRx.exec('xabbbc')[1] + '&t=' + cjTpl + '&c=' + (new CjC()[cjKey]() + CjC[cjKey + 's'])" \
    " + '&n=' + cjN);"

static const char HTML_COMPILEJOIN[] =
    "<!doctype html><html><body>"
    CJ_SINK
    MS_MODULE_OPEN CJ_BODY("/api/cj-mod") "</script>"
    TF_CANVAS_INK
    "</body></html>";

static const char HTML_COMPILEJOINCLASSIC[] =
    "<!doctype html><html><body>"
    CJ_SINK
    "<script>" CJ_BODY("/api/cjc-cls") "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
