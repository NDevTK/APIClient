/* THE TOKEN LIST DOCUMENT: DOM §7.1's DOMTokenList over values a flow has not determined.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_TOKENLIST_H
#define ENGINE_HOST_FIXTURE_TOKENLIST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_TOKENLIST[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    /* DOM §7.1's `value` SETTER, WHICH IS §4.9 "set an attribute value" AND NOT §7.1's UPDATE STEPS — two
       algorithms that shared one body here, and the shared body carried the update steps' step 1. The setter
       steps are quoted whole from the fetched standard: "The value setter steps are to set an attribute value
       for this's element using this's attribute name and the given value", with no early return anywhere in
       them. So assigning the EMPTY STRING to an element that carries no `class` attribute must CREATE
       `class=""`, where the update steps' step 1 ("if get an attribute by namespace and local name ... returns
       null and set's token set is empty, then return") correctly returns and writes nothing.
       A FRESH ELEMENT ON PURPOSE: an element that already has the attribute cannot observe a CREATE. */
    "var clv = document.createElement('i');"
    "clv.classList.value = '';"
    "fetch('/api/clsetempty?v=' + (clv.hasAttribute('class') && clv.getAttribute('class') === ''"
    " ? 'created' : 'wrong'));"

    /* …AND THE SAME SETTER OVER UNKNOWN EXTERNAL INPUT. `setAttribute` hands the DOM chokepoint the SOURCE
       beside the shape's bytes, so `el.setAttribute('class', x)` keeps x's identity in the (element, name)
       taint shadow and a sink fed from the attribute still names a source. `classList.value = x` must do the
       same; writing the bytes with no taint loses the provenance §@S's search starts from, silently.
       THE WITNESS IS A FORK, because nothing aborts here either way: `?v=fork` is learned only if
       `getAttribute` handed back a value carrying its domain, and `?v=other` under either reading — the armed
       control whose absence says the statement never ran.
       `getAttribute` and not `className`, because core/dom/element.c's `js_el_get_attribute` answers the
       shadow first; the `class` reflection's route is a separate question.
       `.slice(1)` because a non-empty `location.hash` always begins with `#`, so `=== 'admin'` over the hash
       itself is infeasible and the engine would prune that arm correctly. */
    "var clt = document.createElement('u');"
    "clt.classList.value = location.hash.slice(1);"
    "if (clt.getAttribute('class') === 'admin') fetch('/api/cltaint?v=fork');"
    "else fetch('/api/cltaint?v=other');"

    /* …AND THE SAME SOURCE THROUGH THE MUTATION FAMILY, a different composition. §7.1's update steps write
       "the result of running the ordered set serializer for set's token set", so the value is several tokens
       of which only some are an argument's shape, while the (element, name) shadow holds ONE JSValue for the
       attribute; `add` composes them through the engine's concatenation (solver/concolic.h's add hook).
       TWO ELEMENTS, TWO ROWS: `cla` has no `class`, so the value is the argument's shape alone; `clb` carries a
       concrete token first, so the composition hands the hook a concrete left operand.
       `=== 'base admin'` because `clb`'s value begins with the concrete token; an equality ignoring that
       prefix would be pruned correctly.
       BOTH READS ARE BRANCHED AND BOTH ARMS EMIT: `?v=lone` and `?v=pair` are learned only if `getAttribute`
       handed back a value carrying its domain, and `?v=flat` and `?v=bare` under either reading, each its own
       row's armed control. */
    "var cla = document.createElement('u');"
    "cla.classList.add(location.hash.slice(1));"
    "if (cla.getAttribute('class') === 'admin') fetch('/api/claddtaint?v=lone');"
    "else fetch('/api/claddtaint?v=flat');"
    "var clb = document.createElement('u');"
    "clb.setAttribute('class', 'base');"
    "clb.classList.add(location.hash.slice(1));"
    "if (clb.getAttribute('class') === 'base admin') fetch('/api/claddtaint?v=pair');"
    "else fetch('/api/claddtaint?v=bare');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
