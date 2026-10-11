/* THE REMOVING-STEPS ORDER DOCUMENT: `replaceChildren` that removes an open dialog and inserts another, which DOM
   §4.2.3's replace all orders as "remove all" then "insert". HTML §4.11.4's dialog cleanup steps destroy the
   removed dialog's close watcher before the inserted dialog's setup steps establish one, and HTML §6.10.2's
   establish appends a new group only while the manager has fewer groups than it allows. With one allowance
   gained (File System Access's showSaveFilePicker runs the activation notification steps when it resolves)
   and the groups « old » « c », the inserted dialog gets a group of its own, so the next close request (§6.10.2's
   process close watchers takes the last group) closes it and leaves `c` open. Run in the other order it joins
   `c`'s group and both close. The record says, on the arm that gained the allowance, whether `c` was still open
   when the inserted dialog's `close` event ran. One of test_forced.c's exploring documents (its TF_DOCS table).
   It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FRAGDIALOG_H
#define ENGINE_HOST_FIXTURE_FRAGDIALOG_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FRAGDIALOG[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    "function fdDialog() { var d = document.createElement('dialog'); d.setAttribute('closedby', 'closerequest');"
    " return d; }"
    "var fdp = document.createElement('div');"
    "document.body.appendChild(fdp);"
    "var fdOld = fdDialog(); fdp.appendChild(fdOld); fdOld.show();"
    "var fdC = fdDialog(); document.body.appendChild(fdC);"
    "var fdNew = fdDialog(); fdNew.setAttribute('open', '');"
    "var fdAct = 0;"
    "fdNew.addEventListener('close', function () {"
    " fetch('/api/fragdialog?s=' + fdAct + (fdC.open ? 'open' : 'shut')); });"
    "function fdRest() { fdC.show(); fdp.replaceChildren(fdNew); fetch('/api/fragdialogready?a=' + fdAct); }"
    "showSaveFilePicker({ suggestedName: 'fd.txt' }).then(function () { fdAct = 1; fdRest(); },"
    " function () { fdRest(); });"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
