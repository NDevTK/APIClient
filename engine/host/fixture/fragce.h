/* THE FRAGMENT CUSTOM ELEMENT DOCUMENT: a defined custom element written through a connected element's innerHTML
   setter. HTML §13.2.6.1's create an element for the token runs no constructor under a fragment context
   (willExecuteScript is false), so the element is upgraded by a reaction DOM §4.2.3's insert enqueues, which runs
   at the setter's [CEReactions] epilogue after the whole fragment is placed: the constructor sees all three
   children. The record carries what it saw. One of test_forced.c's exploring documents (its TF_DOCS table). It
   ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FRAGCE_H
#define ENGINE_HOST_FIXTURE_FRAGCE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FRAGCE[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    "var fceSeen = [];"
    "customElements.define('x-fragce', class extends HTMLElement {"
    " constructor() { super(); fceSeen.push(this.parentNode ? this.parentNode.childNodes.length : -1); } });"
    "var fce = document.createElement('div');"
    "document.body.appendChild(fce);"
    "fce.innerHTML = '<x-fragce></x-fragce><i></i><i></i>';"
    "fetch('/api/fragce?v=' + fceSeen.join('.') + '&u=' + (fce.firstChild instanceof customElements.get('x-fragce')));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
