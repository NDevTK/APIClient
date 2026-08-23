// lib/callsite-url.js — ONE PROBLEM: read the ADDRESS TEMPLATE the engine emits for a fetch call site and say
// which of its URL components the page's code actually determined.
//
// THE PRODUCER'S GRAMMAR, WHICH IS WHERE THIS PARSER COMES FROM. `engine/host/solver/endpoint.c` builds an
// endpoint's `url` as path_scan(url_path_of(url_display(address))): the address as the surface prints it (a
// concolic's display SHAPE, a concrete URL's own bytes), cut at the first `?` — the query is not in this string,
// it is in `params[]` under location "query" — and then re-spelled so that every `/`-separated segment is
// EITHER brace-free literal bytes OR exactly one `{name}` hole covering the whole segment, with `name` free of
// `{`, `}` and `/`. A hole is therefore atomic and segment-wide; there is no such thing as half a hole.
//
// WHAT THAT MAKES THE CONSUMER'S QUESTION. A URL record's ORIGIN is a tuple of its scheme, host and port
// (WHATWG URL §4.7 Origin: for "http"/"https", "Return the tuple origin (url's scheme, url's host, url's port,
// null)"), and its PATH is a separate component (§4.1 URL representation). So a hole standing anywhere in the
// scheme/host/port region means the ORIGIN IS NOT DETERMINED, while a hole in the path region leaves the origin
// known and templates only the path. Those are two different facts about the page and each is a POSITIVE
// statement — never a reason to drop the record.
//
// THE REGEX THIS REPLACES WAS A MATCH ON THREE SPELLINGS, TWO OF WHICH NO PRODUCER WRITES. lib/merge.js and
// lib/learn.js each carried `/^\$\{|^\(dynamic\)|^\{[a-zA-Z]/` — one fact answered in two places, by pattern
// rather than by grammar. `${` and `(dynamic)` are shapes this engine has never emitted; `^\{[a-zA-Z]` caught
// the live case and MISSED `{}` (endpoint.c's unnameable hole, which then resolved against the page base into a
// literal `/%7B%7D` endpoint) and missed a hole in the AUTHORITY of an otherwise-literal address
// (`https://{h}/p`), which it handed to `new URL` as though the host were known.

/* URL §4.4 URL parsing — scheme start state ("If c is an ASCII alpha") then scheme state ("If c is an ASCII
   alphanumeric, U+002B (+), U+002D (-), or U+002E (.)"), terminated by U+003A (:). */
var _AST_SCHEME_RE = /^[A-Za-z][A-Za-z0-9+.\-]*:/;

/* DOES THIS ADDRESS HOLD A HOLE AT ALL — the question "is it concretely fetchable", asked in path_scan's own
   grammar: a hole's name may hold anything except `{`, `}` and `/`. bridge.js asked it as `/\{[a-z]*\}/`, which
   answers NO for every shape this engine actually emits — `{arg0}` has a digit, `{location.hash}` a dot,
   `{orphan3.arg0.replace()}` all three — so the gate that exists to keep an undetermined address away from
   safeFetch matched only `{}` and `{id}`, and the rest went out to somebody's server as a percent-encoded
   literal path. */
function astAddressHasHole(s) {
  return typeof s === "string" && /\{[^{}\/]*\}/.test(s);
}

/* WHERE EVERYTHING THE ORIGIN COULD BE MADE OF HAS ENDED — the index at or after which a hole no longer touches
   scheme, host or port.
   With a scheme and `//` (URL §4.4 authority state -> host state), the authority runs to the first `/`, `?` or
   `#`, which is where path start state begins. With a scheme and no `//` the path is opaque (§4.4 opaque path
   state) and carries no host at all, so nothing after the scheme is origin. With NO scheme the address is
   relative and takes its origin from the base — but only if its FIRST code point is literal, because a hole at
   index 0 can itself supply `https://host`, so the bound is 1. */
function astAddressOriginEnd(template) {
  var m = _AST_SCHEME_RE.exec(template);
  var i, j, c;
  if (!m) return 1;
  i = m[0].length;
  if (template.charAt(i) !== "/" || template.charAt(i + 1) !== "/") return i;
  for (j = i + 2; j < template.length; j++) {
    c = template.charAt(j);
    if (c === "/" || c === "?" || c === "#") return j;
  }
  return template.length;
}

/* THE ATOMICITY path_scan PROMISES, ASSERTED WHERE THIS FILE STARTS RELYING ON IT. Every split below reads a
   hole as a whole segment; a producer that ever emitted a partial one would silently re-cut every address here.
   endpoint.c asserts the same contract from its own side (the path param's NAME may hold no brace and no
   slash), so the two halves of it now crash on the same day rather than one of them going quiet. */
function astAddressCheckGrammar(template) {
  var segs = template.split("/"), i, s;
  for (i = 0; i < segs.length; i++) {
    s = segs[i];
    if (s.indexOf("{") < 0 && s.indexOf("}") < 0) continue;
    DCHECK(s.charAt(0) === "{" && s.charAt(s.length - 1) === "}" &&
           s.indexOf("{", 1) < 0 && s.indexOf("}") === s.length - 1,
           "an @H address template holds a segment that is neither literal nor one whole hole (" +
           JSON.stringify(s) + " in " + JSON.stringify(template) + ") — endpoint.c's path_scan re-spells a " +
           "segment containing a brace as exactly {name} with the braces stripped out of name, so a partial " +
           "hole is that serializer having changed grammar and every split in lib/callsite-url.js reading the " +
           "address at the wrong boundary");
  }
}

/* THE STATEMENT: what this call site's address IS, as far as the code determined it. Always answers; never null.
     { originKnown: true,  url: URL, host, path }  — scheme/host/port are literal, holes (if any) are in the path
     { originKnown: false, url: null, host, path }  — the origin is a SHAPE; `host` is that shape verbatim and
                                                      `path` is the literal remainder beside it (often "")
   `host + path` reconstructs the address in both cases, which is the invariant the popup's endpoint line and
   lib/merge.js's dedup key are both built on.
   A LITERAL ADDRESS THE URL PARSER REJECTS IS NOT HANDLED HERE AND MUST NOT BE — see the DFAIL. */
function astCallSiteAddress(template, base) {
  var end, close, host, path, url;

  DCHECK(typeof template === "string" && template !== "",
         "astCallSiteAddress was given no address template — a call site with no resolved address carries " +
         "url:null and is a structural candidate, refused by its caller before it reaches this parser");
  astAddressCheckGrammar(template);

  end = astAddressOriginEnd(template);
  if (template.indexOf("{") >= 0 && template.indexOf("{") < end) {
    if (_AST_SCHEME_RE.test(template)) {
      host = template.slice(0, end);
      path = template.slice(end);
    } else {
      close = template.indexOf("}");
      DCHECK(close > 0, "an @H address template starts with an unterminated hole (" + JSON.stringify(template) +
             ") — path_scan writes both braces of a hole it mints, so a lone `{` is that producer broken");
      host = template.slice(0, close + 1);
      path = template.slice(close + 1);
    }
    return { originKnown: false, url: null, host: host, path: path };
  }

  /* Fetch §5.4 Request class, new Request(input, init): "Let parsedURL be the result of parsing input with
     baseURL. If parsedURL is failure, then throw a TypeError." A request the page could never issue is a
     request the engine must not have recorded as an endpoint, and a base that is absent is a document with no
     address — neither is a malformed call site to skip past. */
  try {
    url = _AST_SCHEME_RE.test(template) ? new URL(template) : new URL(template, base);
  } catch (e) {
    DFAIL("an @H call-site address whose origin is fully literal does not parse as a URL (" +
          JSON.stringify(template) + " against base " + JSON.stringify(base) + ") — per Fetch §5.4 the page's " +
          "own fetch() would have thrown a TypeError on it, so endpoint.c recorded an endpoint for a request " +
          "no browser can make, or this merge was handed neither the document's url nor the analysis " +
          "sourceUrl to resolve a relative address against: " + (e && e.message ? e.message : e));
    throw e;
  }
  return { originKnown: true, url: url, host: url.hostname, path: _decHoles(url.pathname) };
}
