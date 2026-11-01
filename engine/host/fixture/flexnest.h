/* THE NESTED FLEX DOCUMENTS: css-flexbox-1 §9.6 "Cross-Axis Alignment"' content-based cross size of `row` flex
   containers nested one inside the next, three deep (`flexnest3`), six deep (`flexnest6`) and ten deep
   (`flexnest10`). Three of test_forced.c's exploring documents (its TF_DOCS table). Each level holds a 5px by 1px item and then the next
   level as its second item; the innermost holds a 5px by 1px item and a 5px by 10px one. So every line is 10px
   tall (§9.4 step 8's largest outer hypothetical cross size), `#fo`'s scrolling area is 10px tall, and the 10px
   item starts 5px per level from the left (§9.7 resolves no flexible length: every item fits at its basis).
   Every level but the outermost is a flex item whose `width` is `auto`, so its §9.2 flex base size is §9.9.1's
   max-content size of the level inside it, and each level's §9.3 lines read every level below.
   CSSOM VIEW §7's `offsetLeft` and `offsetTop` and §6's `scrollHeight` are read under a resting drive, which
   asks every level's cross size, a layout job question answered once per job. The numbers are derived from
   those sections, not read off a browser. They end with TF_CANVAS_INK, which test_forced.c defines before
   including it. */
#ifndef ENGINE_HOST_FIXTURE_FLEXNEST_H
#define ENGINE_HOST_FIXTURE_FLEXNEST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define FN_OPEN "<div style=\"display:flex\"><div style=\"width:5px;height:1px\"></div>"
#define FN_LEAF "<div id=fl style=\"width:5px;height:10px\"></div>"
#define FN_CLOSE "</div>"
#define FN_DOC(name, levels, closes)                                                                          \
    "<!doctype html><html><body style=\"margin:0\">"                                                          \
    "<div id=fo style=\"width:200px;overflow:hidden\">" levels FN_LEAF closes "</div>"                        \
    "<script>var o=document.getElementById('fo'),l=document.getElementById('fl');"                           \
    "fetch('/api/" name "?h='+o.scrollHeight+'&l='+l.offsetLeft+'&t='+l.offsetTop);</script>"                 \
    TF_CANVAS_INK "</body></html>"

static const char HTML_FLEXNEST3[] = FN_DOC("flexnest3", FN_OPEN FN_OPEN FN_OPEN, FN_CLOSE FN_CLOSE FN_CLOSE);
static const char HTML_FLEXNEST6[] =
    FN_DOC("flexnest6", FN_OPEN FN_OPEN FN_OPEN FN_OPEN FN_OPEN FN_OPEN,
           FN_CLOSE FN_CLOSE FN_CLOSE FN_CLOSE FN_CLOSE FN_CLOSE);
static const char HTML_FLEXNEST10[] =
    FN_DOC("flexnest10", FN_OPEN FN_OPEN FN_OPEN FN_OPEN FN_OPEN FN_OPEN FN_OPEN FN_OPEN FN_OPEN FN_OPEN,
           FN_CLOSE FN_CLOSE FN_CLOSE FN_CLOSE FN_CLOSE FN_CLOSE FN_CLOSE FN_CLOSE FN_CLOSE FN_CLOSE);

/* THE WRAPPING FLEX DOCUMENT (`linecrossrest`): a multi-line `row` flex container 40px wide given forty 3px
   items of heights 1px, 2px, 3px in turn and then `#fz`, 3px by 2px. css-flexbox-1 §9.3 "Main Size
   Determination"' step 5 collects thirteen items (39px) per line, so the fourth line holds the fortieth item
   (1px) and `#fz`; §9.4 "Cross Size Determination"' step 8 makes the first three lines 3px and the fourth 2px.
   So `#fz` is 9px down and 3px in, and `#fc`'s scrolling area is 11px tall. The script builds the items, so
   CSSOM VIEW §7's `offsetTop` lays them out under a resting drive that rests between its boxes, and the line's
   cross size is served across those rests more often than any §9.4.1 contribution is served at all. */
static const char HTML_LINECROSSREST[] =
    "<!doctype html><html><body style=\"margin:0\">"
    "<div id=fc style=\"display:flex;flex-wrap:wrap;width:40px\"></div>"
    "<script>var c=document.getElementById('fc'),i,z;"
    "for(i=0;i<41;i++){z=document.createElement('div');"
    "z.style.cssText='width:3px;height:'+(i<40?1+i%3:2)+'px';c.appendChild(z);}"
    "fetch('/api/linecrossrest?t='+z.offsetTop+'&l='+z.offsetLeft+'&h='+c.scrollHeight);"
    "</script>" TF_CANVAS_INK "</body></html>";

#undef FN_OPEN
#undef FN_LEAF
#undef FN_CLOSE
#undef FN_DOC

#endif
