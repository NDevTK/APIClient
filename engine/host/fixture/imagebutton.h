/* THE IMAGE BUTTON DOCUMENT: HTML §4.10.5.1.19 "Image Button state (type=image)"'s image fetch and HTML
   §15.4.2 "Images"' rules 1 and 2 for an `input`, read through offsetWidth/offsetHeight. A parsed button and a
   script-made one load a 2x3 PNG and are 2x3 at their `load`; a parsed button with no alt whose image is
   corrupt fires `error`, and one with no src and no alt fires nothing; both are rule 2's replaced element
   with no natural dimensions, CSS 2.1 §10.3.2's 300x150. A fourth names an address the surface records under
   the `image-input` door. Chrome 153.0.8010.12 gives every event and every rule 1 size expected; for the two
   rule 2 boxes it gives 57x16, not the specification's 300x150, as it gives a broken `img` with no alt 16x16
   where core/layout/replaced_element.c's `img` arm answers 300x150 too.
   Chrome also fires `load` twice at the parsed 2x3 button, running the trigger once for `type` with `src`
   already present and once for `src`, where HTML §13.2.6.1 "Creating and inserting nodes" appends a token's
   attributes one at a time, so §4.10.5.1.19's trigger runs once.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_IMAGEBUTTON_H
#define ENGINE_HOST_FIXTURE_IMAGEBUTTON_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define IMAGEBUTTON_PNG "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAADCAYAAAC56t6BAAAAEUlEQVR4nGP4z8DwH4QZMBgAoXkL9U3EmgcAAAAASUVORK5CYII="

static const char HTML_IMAGEBUTTON[] =
    "<!doctype html><html><body><form id=ibF action='/api/ibform'>"
    "<input type=image id=ibA src=\"" IMAGEBUTTON_PNG "\">"
    "<input type=image id=ibB src=\"data:image/png;base64,AAAA\">"
    "<input type=image id=ibC>"
    "<input type=image id=ibD src='/api/ibsrc.png'>"
    "</form>"
    "<script>"
    "function sz(e){return e.offsetWidth+'x'+e.offsetHeight;}"
    "function watch(e,u){"
    "e.addEventListener('load',function(){fetch(u+'?v=load:'+sz(e));});"
    "e.addEventListener('error',function(){fetch(u+'?v=error:'+sz(e));});}"
    "watch(document.getElementById('ibA'),'/api/ibA');"
    "watch(document.getElementById('ibB'),'/api/ibB');"
    "var ibE=document.createElement('input');ibE.type='image';ibE.src='" IMAGEBUTTON_PNG "';"
    "watch(ibE,'/api/ibE');document.getElementById('ibF').appendChild(ibE);"
    "addEventListener('load',function(){fetch('/api/ibC?v='+sz(document.getElementById('ibC')));});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#undef IMAGEBUTTON_PNG
#endif
