/* THE CONTROL'S BODY. Served at 200, so HTML §8.1.4.2 "Fetching scripts"' "fetch a classic script" step 5.2
   admits it, step 5.8 runs onComplete with a script, and HTML §4.12.1.1 "Processing model"'s "execute the
   script element" reaches its last step: "If el's from an external file is true, then fire an event named load
   at el."
   This request is the proof that the whole chain — fetch, decode, compile, run — works in this run, which is
   what makes the OTHER script's silence a reading rather than an absence. */
fetch("/scrstat-ctl-body-ran.txt");
