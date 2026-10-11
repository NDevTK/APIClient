/* THE SCRIPTS DOCUMENT: HTML §4.12.1's script order and §3.1.7's currentScript.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SCRIPTS_H
#define ENGINE_HOST_FIXTURE_SCRIPTS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SCRIPTS[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    /* HTML §4.12.1.1 "Processing model": an inline script that is not parser-inserted runs inside the
       appendChild that connects it. DOM §4.2.3 "Mutation algorithms"' insert runs the post-connection steps
       before it returns, HTML's script post-connection steps "Prepare the script element given insertedNode",
       and prepare's last step is "Otherwise, immediately execute the script element el, even if other scripts
       are already executing." `__so` is appended to by each program in turn and the next <script> reports it:
       `AXBC` is that order; `ABXC` is the injected program deferred to after its injector, and `ABC` is it
       taken to the tail, after the program that reports. */
    "var __so = 'A';"
    "var soEl = document.createElement('script');"
    "soEl.textContent = \"__so += 'X';\";"
    "document.body.appendChild(soEl);"
    "__so += 'B';"
    "</script>"
    /* THE REPORTING <script>, and it exists only to REPORT — see the injection in script 1. It has
       to be a document script rather than a task, because what is being proved is that a program injected from
       a NON-FINAL <script> took a position ahead of the document's remaining ones. */
    "<script>__so += 'C'; fetch('/api/scriptorder?seq=' + __so);</script>"
    /* HTML §3.1.7 "DOM tree accessors"'s `currentScript`, and the §4.12.1.1 "Processing model" bracket that
       writes it. THE FIXTURE PROVES THE DESIGN AND NOT THE MEMBER: a getter that reads a slot is one line, and
       what is hard here is that "run the classic script" is a WORK ITEM — it begins at one scheduler step and
       ends at another, with sibling flows running programs of their own in between — so a C save/restore
       bracket around the compile would answer one flow's element to another and would pass any fixture that
       runs a single script.
       `csB` FORKS ON UNKNOWN INPUT AND ONE ARM INJECTS A SCRIPT, which is what makes the two arms run
       DIFFERENT elements at the same cursor: the admin arm's `csINJ` runs at the slot §4.12.1.1's "immediately
       execute the script element" names, while the other arm is at `csC`. Under a slot that is not per-flow,
       whichever arm ran second reads the other's element and `/api/csend` reports `csINJ`.
       THE READ IS `getAttribute` AND THE FETCH IS BUILT FROM IT, because that is the real shape: nodejs.org's
       chunk preamble derives its asset prefix from `document.currentScript.getAttribute("src")`, so an
       undefined `currentScript` computes every lazy-chunk URL wrong — the moat surface, not a nicety. */
    "<script id=csA>window.__cs = document.currentScript.getAttribute('id') + ':' +"
    " (document.currentScript.getAttribute('src') === null ? 'inline' : 'src');</script>"
    "<script id=csB>"
    "if (state.admin) { var csi = document.createElement('script'); csi.setAttribute('id', 'csINJ');"
    " csi.textContent = \"fetch('/api/csinj?el=' + document.currentScript.getAttribute('id'));\";"
    " document.body.appendChild(csi); }"
    /* §3.1.7: "Returns null if the Document is not currently executing a script … (e.g., because the running
       script is an event handler, or a timeout)". This is step 4's restore observed from OUTSIDE the bracket:
       the timer callback is a task of this flow that runs after every program of the document has completed,
       so a slot that was never restored reports the last element that ran instead of null. */
    "setTimeout(function(){ fetch('/api/cstimer?el=' +"
    " (document.currentScript === null ? 'null' : document.currentScript.getAttribute('id'))); }, 0);"
    "</script>"
    /* §4.12.1.1's MODULE arm asserts `currentScript` is null and never sets it — the standard's own assertion,
       which is a DCHECK at the module compile and is observable here. `import.meta` is what a module reads
       instead, which is why the member is not merely unset but has to be NULL. */
    "<script type=module>fetch('/api/csmod?el=' +"
    " (document.currentScript === null ? 'null' : 'LEAK'));</script>"
    "<script id=csC>fetch('/api/csend?el=' + __cs + '|' +"
    " document.currentScript.getAttribute('id'));</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
