/* THE GRID SELF-ALIGNMENT DOCUMENT: css-grid-1 §10.3 "Inline-axis Alignment: the justify-self and justify-items
   properties" and §10.4 "Block-axis Alignment: the align-self and align-items properties" placing a 20 x 10
   margin box in a 100 x 80 grid area by css-align-3's positional keywords. One of test_forced.c's exploring
   documents (its TF_DOCS table). Each row is one container, one item; the reported pair is the item's offset
   from its container's content box. Covered: start, end, center and their flex aliases on both axes, `left`
   and `right`, `normal` and `stretch` on a definite size (start), `align-items` and `justify-items` inherited
   through `auto`, a 120px-wide item centred unsafely and `safe`ly, and an rtl container's start and end.
   Chrome 155.0.8059.84 and Firefox 158.0.1 give every value expected. It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_GRIDALIGN_H
#define ENGINE_HOST_FIXTURE_GRIDALIGN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define GRIDALIGN_ROW(id, cstyle, istyle) \
    "<div class=g style=\"" cstyle "\"><div id=" id " class=i style=\"" istyle "\"></div></div>"

static const char HTML_GRIDALIGN[] =
    "<!doctype html><html>"
    "<head><style>"
    ".g{display:grid;grid-template-columns:100px;grid-template-rows:80px;width:100px}"
    ".i{width:20px;height:10px}"
    "</style></head>"
    "<body style=\"margin:0\">"
    GRIDALIGN_ROW("a1", "", "align-self:start;justify-self:start")
    GRIDALIGN_ROW("a2", "", "align-self:end;justify-self:end")
    GRIDALIGN_ROW("a3", "", "align-self:center;justify-self:center")
    GRIDALIGN_ROW("a4", "", "align-self:flex-end;justify-self:flex-end")
    GRIDALIGN_ROW("a5", "", "align-self:flex-start;justify-self:right")
    GRIDALIGN_ROW("a6", "", "justify-self:left")
    GRIDALIGN_ROW("a7", "", "align-self:stretch;justify-self:stretch")
    GRIDALIGN_ROW("a8", "align-items:center;justify-items:end", "")
    GRIDALIGN_ROW("a9", "", "width:120px;justify-self:center")
    GRIDALIGN_ROW("a10", "", "width:120px;justify-self:safe center")
    GRIDALIGN_ROW("a11", "direction:rtl", "justify-self:start")
    GRIDALIGN_ROW("a12", "direction:rtl", "justify-self:end")
    GRIDALIGN_ROW("a13", "", "margin:5px 3px;align-self:end;justify-self:end")
    "<script>"
    "function O(i){var e=document.getElementById(i),r=e.getBoundingClientRect(),"
    "p=e.parentElement.getBoundingClientRect();return (r.left-p.left)+'_'+(r.top-p.top);}"
    "var q='',i;for(i=1;i<=13;i++)q+=(i>1?'&':'')+'a'+i+'='+O('a'+i);"
    "fetch('/api/gridalign?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef GRIDALIGN_ROW
#endif
