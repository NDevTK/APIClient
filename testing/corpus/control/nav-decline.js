// RUNG 2 — SCRIPT NAMES IT. The element is created and inserted with no `src`, so §4.8.5 processes its
// attributes and finds nothing to fetch; the ASSIGNMENT below is an attribute change, which runs "process the
// iframe attributes" a second time and is the route a bundle actually uses. It is a DIFFERENT path through
// core/html/html_iframe.c from the parser's and reaches the same §7.4 step 14 load.
//
// THE ADDRESS IS A CONSTANT AND THAT IS DELIBERATE. A composed address would make this rung ALSO a test of
// whether the witness mark is `pinned`, which is a second question and the one #117 is about — and a `pinned`
// address is refused by a DIFFERENT conjunct, so a reader could not tell which conjunct held it. One question
// per rung.
var f = document.createElement("iframe");
f.id = "scripted";
f.width = "200";
f.height = "60";
document.body.appendChild(f);
f.src = "/nav/child.html";

// AND NO `fetch()` ANYWHERE IN THIS DOCUMENT, WHICH IS A PROPERTY THE CENSUS ORACLE NEEDS RATHER THAN AN
// OMISSION. `fork-over-a-declined-request > 0` with `egressDeclined == {}` is the signature of a decline that
// did not come through the fetch seam; one fetch refusal can raise the fork count several times, so a single
// `fetch()` here would make that signature unreadable. The server's access log is the primary oracle and this
// keeps the secondary one clean.
