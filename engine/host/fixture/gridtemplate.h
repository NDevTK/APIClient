/* THE GRID TEMPLATE DOCUMENT: css-grid-1 §7.4 "Explicit Grid Shorthand: the grid-template property" expanded
   into its three longhands, laid out, and serialized back through CSSOM. One of test_forced.c's exploring
   documents (its TF_DOCS table). #g carries the declaration a real page's root grid does,
   `"header" auto "content" minmax(0,1fr) / minmax(0,1fr)`: a 10px header placed by its area name over an `auto`
   row, and the content over a `minmax(0, 1fr)` row taking the other 50px of 60. The four serializations are
   each compared in the page against the string Chrome 155.0.8059.84 and Firefox 158.0.1 both give, since a
   serialization carries quotes a fetch parameter cannot be matched on: the ASCII-art form drops an `auto` size
   and writes `0` as `0px`, the rows/columns form keeps `none` areas, a value outside the grammar sets nothing
   (`bogus` is no track size), and line names between two rows are spliced into one list. It ends with
   TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_GRIDTEMPLATE_H
#define ENGINE_HOST_FIXTURE_GRIDTEMPLATE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_GRIDTEMPLATE[] =
    "<!doctype html><html>"
    "<body style=\"margin:0\">"
    "<div id=g style='display:grid;width:100px;height:60px;"
    "grid-template:\"header\" auto \"content\" minmax(0,1fr) / minmax(0,1fr)'>"
    "<div id=h style=\"grid-area:header;height:10px\"></div><div id=c style=\"grid-area:content\"></div></div>"
    "<div id=g2></div><div id=g3></div><div id=g4></div>"
    "<script>"
    "function E(i){return document.getElementById(i);}"
    "function Q(v,want){return v===want?'ok':'wrong';}"
    "var s=E('g').style;"
    "E('g2').style.gridTemplate='auto 1fr / 10px 20px';"
    "E('g3').style.gridTemplate='\"a\" \"b\" bogus';"
    "E('g4').style.gridTemplate='[x] \"a\" 10px [y] [z] \"b\" / 5px';"
    "var q='ht='+E('h').offsetTop+'&hh='+E('h').offsetHeight+'&ct='+E('c').offsetTop"
    "+'&ch='+E('c').offsetHeight+'&cw='+E('c').offsetWidth"
    "+'&st='+Q(s.gridTemplate,'\"header\" \"content\" minmax(0px, 1fr) / minmax(0px, 1fr)')"
    "+'&sr='+Q(s.gridTemplateRows,'auto minmax(0px, 1fr)')+'&sc='+Q(s.gridTemplateColumns,'minmax(0px, 1fr)')"
    "+'&sa='+Q(s.gridTemplateAreas,'\"header\" \"content\"')"
    "+'&s2='+Q(E('g2').style.gridTemplate,'auto 1fr / 10px 20px')+'&s2a='+Q(E('g2').style.gridTemplateAreas,'none')"
    "+'&s3='+Q(E('g3').style.gridTemplate,'')+'&s3r='+Q(E('g3').style.gridTemplateRows,'')"
    "+'&s4='+Q(E('g4').style.gridTemplate,'[x] \"a\" 10px [y z] \"b\" / 5px')"
    "+'&s4r='+Q(E('g4').style.gridTemplateRows,'[x] 10px [y z] auto')"
    "+'&raw='+encodeURIComponent(s.gridTemplate+'|'+E('g4').style.gridTemplate);"
    "fetch('/api/gridtemplate?'+q);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
