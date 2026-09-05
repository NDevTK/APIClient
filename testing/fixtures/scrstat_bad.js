/* THE BODY THAT MUST NEVER RUN. It is served under a NON-OK STATUS, and HTML §8.1.4.2 "Fetching scripts"'
   "fetch a classic script" step 5.2 refuses it on the status alone — the step opens "If any of the following
   are true:" over a two-item list whose second member is "response's status is not an ok status", and then
   "run onComplete given null, and abort these steps". Fetch §2.2.3 "Statuses" is the only definition of that
   member: "An ok status is a status in the range 200 to 299, inclusive."
   The element's result is therefore null, and HTML §4.12.1.1 "Processing model"'s "execute the script element"
   step 4 is what the page sees: "If el's result is null, then fire an event named error at el, and return."
   IT IS DELIBERATELY VALID, WELL-TYPED JAVASCRIPT, and that is the whole design of this file. A real 404 page
   is HTML, so a run in which it did not execute would be consistent with TWO causes — the status refusal, and
   a SyntaxError over markup — and a fixture whose one observation has two mechanisms behind it cannot direct
   anything. Served as `application/javascript` with a body a compiler accepts, "did not run" has exactly one
   remaining cause: the status. §8.1.4.2 is explicit that the type is not the question here — "For historical
   reasons, this algorithm does not include MIME type checking". */
fetch("/scrstat-404-body-ran.txt");
