/* THE IMAGE PENDING REQUEST DOCUMENT: HTML §4.8.4.3.5 "Updating the image data" on an img whose `src` is set
   again after its first `load` — the shape a real page's cover image takes — so the second request is the
   PENDING request and the first stays current until the second settles. One of test_forced.c's exploring
   documents (its TF_DOCS table). A 1x1 PNG loads; its load handler sets a 2x3 PNG and, a microtask later, the
   same address again, which §4.8.4.3.5 returns from ("If the pending request is not null and urlString is the
   same as the pending request's current URL"); the second load sees the upgraded request, and a third,
   corrupt source is upgraded to a broken current request. Chrome 155.0.8059.84 and Firefox 158.0.1 give every
   value expected. Cross-engine notes, left out: in the microtask after the second `src` Firefox reports the
   pending request in flight (`complete` false, `currentSrc` the first image) as §4.8.4.3.5 reads, while
   Chrome has already decoded the data: URL; and both report `complete` false synchronously after the set,
   before the microtask that makes the pending request. It ends with TF_CANVAS_INK, which test_forced.c defines
   before including it. */
#ifndef ENGINE_HOST_FIXTURE_IMGPENDING_H
#define ENGINE_HOST_FIXTURE_IMGPENDING_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define IMGPENDING_A "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg=="
#define IMGPENDING_B "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAADCAYAAAC56t6BAAAAEUlEQVR4nGP4z8DwH4QZMBgAoXkL9U3EmgcAAAAASUVORK5CYII="

static const char HTML_IMGPENDING[] =
    "<!doctype html><html><body>"
    "<img id=i src=\"" IMGPENDING_A "\">"
    "<script>"
    "var A='" IMGPENDING_A "',B='" IMGPENDING_B "',C='data:image/png;base64,AAAA';"
    "function N(u){return u===A?'A':u===B?'B':u===C?'C':u===''?'empty':'other';}"
    "var i=document.getElementById('i'),q={},loads=0,errors=0,step=0;"
    "function fin(){fetch('/api/imgpending?l1='+q.l1+'&l2='+q.l2+'&err='+q.err+'&n='+loads+'_'+errors);}"
    "i.addEventListener('error',function(){errors++;q.err=N(i.currentSrc)+':'+i.naturalWidth+':'+i.complete;fin();});"
    "i.addEventListener('load',function(){loads++;"
    "if(step===0){step=1;q.l1=i.naturalWidth+'x'+i.naturalHeight+':'+N(i.currentSrc);i.src=B;"
    "Promise.resolve().then(function(){i.src=B;});}"
    "else if(step===1){step=2;q.l2=i.naturalWidth+'x'+i.naturalHeight+':'+N(i.currentSrc)+':'+i.complete;i.src=C;}"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef IMGPENDING_A
#undef IMGPENDING_B
#endif
