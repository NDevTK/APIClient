/* THE REJECTION DOCUMENT: HTML §8.1.4.7's unhandled rejections and their retraction.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_REJECTION_H
#define ENGINE_HOST_FIXTURE_REJECTION_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_REJECTION[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    /* …and §8.1.4.7's rejection nobody handles, from a chunk of its own — see TF_CHUNKS for why it is an
       Error from a lazily-loaded program rather than a bare string from this one. */
    "loadScript('/chunk/rejthrow.js');"
    /* …and §8.1.6.4 step 7.4's half, which is what a report this engine can TAKE BACK is for. Four states come
       out of one row's arithmetic — see TF_CHUNKS for what each of them is and why they are Errors from a
       chunk rather than strings from here. */
    "loadScript('/chunk/rejretract.js');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
