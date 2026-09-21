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
   `{orphan3a153db70eec69f2.arg0.replace()}` all three — so the gate that exists to keep an undetermined
   address away from safeFetch matched only `{}` and `{id}`, and the rest went out to somebody's server as a
   percent-encoded literal path. The orphan spelling here is the LOCATOR one engine_orphan_call actually
   mints; it read `{orphan3.arg0…}` — the ordinal that the cross-session name retired — which is a shape no
   producer writes, i.e. exactly the criticism this paragraph levels at `${` and `(dynamic)` two comments up. */
function astAddressHasHole(s) {
  return typeof s === "string" && /\{[^{}\/]*\}/.test(s);
}

/* A HOLE'S NAME IS NOT URL SYNTAX, AND THE ONLY WAY TO SAY THAT TO A URL PARSER IS TO PERCENT-ENCODE IT.
   `path_scan` keeps `{`, `}` and `/` out of a name and nothing else, because a name is a DISPLAY SHAPE and a
   shape carries the page's own bytes verbatim: solver/concolic.c's `derived_operand_shape` renders a concrete
   String operand QUOTED with only `"` and `\` escaped, and `concolic_add_hook` renders one UNQUOTED, so
   `x.split("#")` and `"/api/" + h` put whatever the page wrote between the braces. `new URL` then reads that
   name in a DIFFERENT grammar. URL §4.4 "URL parsing" path state has two arms over a code point, and the
   name meets both:
     - it ENDS the segment when "c is the EOF code point or U+002F (/)", when "url is special and c is
       U+005C (\)", and when "state override is not given and c is U+003F (?) or U+0023 (#)" — after which
       "If c is U+0023 (#), then set url's fragment to the empty string and state to fragment state";
     - otherwise it runs "UTF-8 percent-encode c using the path percent-encode set and append the result to
       buffer", and URL §1.3 "Percent-encoded bytes" makes that set "the query percent-encode set and
       U+003F (?), U+005E (^), U+0060 (`), U+007B ({), and U+007D (})" over "the C0 control percent-encode set
       and U+0020 SPACE, U+0022 ("), U+0023 (#), U+003C (<), and U+003E (>)", itself "C0 controls and all code
       points greater than U+007E (~)".
   `_decHoles` reverses the SECOND arm for exactly two code points, `%7B` and `%7D`, and reverses the first
   for none — which is why a hole's BRACES come back and the name between them does not.
   MEASURED, `new URL(t, "https://example.com/")` with `_decHoles` applied, over U+0000..U+00FF plus U+2028,
   U+4E2D and U+1F600: of 259 code points a name may legally hold, 172 did not come back. `#` and `?`
   TRUNCATE — `api/{a#b}/z` became `api/{a`, losing the hole's tail AND the literal `/z` after it — and `\`
   split one hole across two segments (`api/{a\b}/z` became `api/{a/b}/z`), which is the state
   astAddressCheckGrammar exists to forbid and cannot see, because it runs BEFORE the parse and nothing
   re-checks after. SPACE, `"`, `<`, `>`, backtick, DEL, every C0 control and EVERY non-ASCII code point came
   back percent-encoded, so the path spelled a name the endpoint's own params[] row does not: lib/popup-
   form.js's applyPathParams matches /\{([^}\/]+)\}/ and looks the capture up in pathParams, so `{a%20b}`
   beside a param named `a b` leaves the match untouched and sends `{a%20b}` to the server as a literal.
   SO THE NAME IS MASKED AT THE DOOR AND UNMASKED ON THE WAY OUT, and every scan below is then correct by
   construction rather than by remembering — after `_astMaskHoles` no URL delimiter exists inside a hole
   anywhere in this file, which is what stops the next scanner added here getting it wrong too.
   THE SET IS STATED AS THE SPEC'S RULE AND NOT AS THE 172 CODE POINTS MEASURED, because the two are not one
   list: `^` IS in the path percent-encode set and the engine this was measured on leaves it alone, so an
   enumeration taken off a run describes one parser's lag where the rule describes every parser. `%` is masked
   with them so the inverse is exact — a name holding a literal `%23` masks to `%2523` and comes back `%23`,
   and `_decHoles`'s own `/%7[Bb]/` cannot match inside `%257B` either. The BRACES are never masked: they are
   the span this file reads a hole by, and they are the one pair `_decHoles` already restores. */
function _astHoleMasked(cp) {
  return cp <= 0x20 || cp >= 0x7f ||
         cp === 0x22 || cp === 0x23 || cp === 0x25 || cp === 0x3c ||
         cp === 0x3e || cp === 0x3f || cp === 0x5c || cp === 0x5e ||
         cp === 0x60 || cp === 0x7b || cp === 0x7d;
}

/* IS THIS SEGMENT ONE WHOLE HOLE — astAddressCheckGrammar's own test, asked here so that a hole and a literal
   segment are not told apart two different ways in one file. */
function _astSegIsHole(s) {
  return s.length > 1 && s.charAt(0) === "{" && s.charAt(s.length - 1) === "}";
}

/* ONE NAME, EACH WAY. `encodeURIComponent` IS URL §1.3's percent-encode over the UTF-8 encoding — "return a
   string consisting of U+0025 (%), followed by two ASCII upper hex digits representing byte" — and it covers
   every code point of the set above. It THROWS on a LONE SURROGATE, which a page string may hold, so that one
   code point is passed through rather than handed an abort switch: it is mangled by the parser exactly as it
   is today, no worse, and the pair stays its own inverse for it because neither transform touches it.
   `decodeURIComponent` is the exact inverse of what was written here, so it cannot invent a byte; it is
   guarded because `_decHoles` can make a `{` out of a base the page supplied, and a span that arrives
   malformed is returned UNCHANGED rather than partly decoded. */
function _astHoleMask(name) {
  var out = "", ch;
  for (ch of name) {
    if (!_astHoleMasked(ch.codePointAt(0))) { out += ch; continue; }
    try { out += encodeURIComponent(ch); } catch (e) { out += ch; }
  }
  return out;
}
function _astHoleUnmask(name) {
  try { return decodeURIComponent(name); } catch (e) { return name; }
}

/* THE TEMPLATE WITH EVERY HOLE MADE INERT, AND ITS INVERSE. Both walk SEGMENTS and not brace spans, because
   segment-atomicity is what astAddressCheckGrammar has just asserted and because a segment walk cannot be
   fooled by a `{` the BASE contributed through `_decHoles`. */
function _astMaskHoles(template) {
  var segs = template.split("/"), i;
  for (i = 0; i < segs.length; i++) {
    if (!_astSegIsHole(segs[i])) continue;
    segs[i] = "{" + _astHoleMask(segs[i].slice(1, -1)) + "}";
  }
  return segs.join("/");
}
function _astUnmaskHoles(masked) {
  var segs = masked.split("/"), i;
  for (i = 0; i < segs.length; i++) {
    if (!_astSegIsHole(segs[i])) continue;
    segs[i] = "{" + _astHoleUnmask(segs[i].slice(1, -1)) + "}";
  }
  return segs.join("/");
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
     { originKnown: true,  url: URL, href, host, path }  — scheme/host/port are literal, holes (if any) are in
                                                           the path, and `href` is the WHOLE address
     { originKnown: false, url: null, href: null, host, path }  — the origin is a SHAPE; `host` is that shape
                                                      verbatim and `path` is the literal remainder beside it
   `host + path` reconstructs the address in both cases, which is the invariant the popup's endpoint line and
   lib/merge.js's dedup key are both built on.
   `href` IS HERE BECAUSE `url.href` IS NO LONGER IT, AND THAT IS THE MASK'S ONE COST. The `url` object is
   parsed from the MASKED template, so its `href` spells a hole's name percent-encoded; the address is that
   href unmasked, and it is minted HERE rather than recomposed at each reader, because a second speller of
   "the address" is exactly what lib/merge.js's own banner says it removed. `url` survives for the
   questions that are about the ORIGIN region — hostname, origin, classifyInterface — which holds no hole on
   this arm and is therefore untouched by the mask.
   A LITERAL ADDRESS THE URL PARSER REJECTS IS NOT HANDLED HERE AND MUST NOT BE — see the DFAIL. */
function astCallSiteAddress(template, base) {
  var masked, end, close, host, path, url;

  DCHECK(typeof template === "string" && template !== "",
         "astCallSiteAddress was given no address template — a call site with no resolved address carries " +
         "url:null and is a structural candidate, refused by its caller before it reaches this parser");
  astAddressCheckGrammar(template);
  /* EVERY SCAN BELOW READS THE MASKED TEMPLATE, INCLUDING THE TWO THAT ARE NOT `new URL`. astAddressOriginEnd
     stops the authority at `/`, `?` or `#`, and a `#` inside a hole ended it mid-name — measured,
     `https://{h#x}/p` answered host `https://{h` and path `#x}/p`, splitting one hole across the two halves
     of the record lib/merge.js keys its endpoints on. That scanner is unchanged and correct now only because
     no delimiter reaches it from inside a hole any more, which is the whole reason the mask is taken once,
     here, rather than at the parse it was written for. */
  masked = _astMaskHoles(template);
  /* THE PAIR IS ITS OWN INVERSE, ASSERTED ON THE REAL TEMPLATE AND NOT ON A FIXTURE. Everything this file
     returns below is read out of `masked`, so a code point the mask writes and the unmask does not read back
     is a name silently re-spelled on the emitted surface — which is the defect the mask exists to end,
     arriving through the mask. It is asserted HERE because both sides are this file's own transform over a
     string it has already grammar-checked, so nothing a page can write makes it fire: `encodeURIComponent`
     is refused only by a LONE SURROGATE, and that one code point is passed through by BOTH halves and so
     compares equal. The condition is O(template) and astAddressCheckGrammar above already walks the same
     segments, so this is the second pass over a call-site address and not a new order of cost. */
  DCHECK(_astUnmaskHoles(masked) === template,
         "masking an @H address template's holes for the URL parse is not reversible (" +
         JSON.stringify(template) + " masked to " + JSON.stringify(masked) + " and came back " +
         JSON.stringify(_astUnmaskHoles(masked)) + ") — every component this parser answers with is read " +
         "out of the masked form, so a code point _astHoleMask writes that _astHoleUnmask does not read " +
         "back is a hole NAME re-spelled on the emitted endpoint, which lib/popup-form.js's applyPathParams " +
         "then fails to match against the param of the same name and sends to the server as a literal");

  end = astAddressOriginEnd(masked);
  if (masked.indexOf("{") >= 0 && masked.indexOf("{") < end) {
    if (_AST_SCHEME_RE.test(masked)) {
      host = masked.slice(0, end);
      path = masked.slice(end);
    } else {
      close = masked.indexOf("}");
      DCHECK(close > 0, "an @H address template starts with an unterminated hole (" + JSON.stringify(template) +
             ") — path_scan writes both braces of a hole it mints, so a lone `{` is that producer broken");
      host = masked.slice(0, close + 1);
      path = masked.slice(close + 1);
    }
    return { originKnown: false, url: null, href: null,
             host: _astUnmaskHoles(host), path: _astUnmaskHoles(path) };
  }

  /* Fetch §5.4 Request class, new Request(input, init): "Let parsedURL be the result of parsing input with
     baseURL. If parsedURL is failure, then throw a TypeError." A request the page could never issue is a
     request the engine must not have recorded as an endpoint, and a base that is absent is a document with no
     address — neither is a malformed call site to skip past. */
  try {
    url = _AST_SCHEME_RE.test(masked) ? new URL(masked) : new URL(masked, base);
  } catch (e) {
    DFAIL("an @H call-site address whose origin is fully literal does not parse as a URL (" +
          JSON.stringify(template) + " against base " + JSON.stringify(base) + ") — per Fetch §5.4 the page's " +
          "own fetch() would have thrown a TypeError on it, so endpoint.c recorded an endpoint for a request " +
          "no browser can make, or this merge was handed neither the document's url nor the analysis " +
          "sourceUrl to resolve a relative address against: " + (e && e.message ? e.message : e));
    throw e;
  }
  /* `href` IS `url.href` PUT BACK THROUGH THE MASK, NEVER RECOMPOSED FROM PARTS. `url.origin + path` is the
     obvious spelling and it is wrong for an OPAQUE PATH: URL §4.7 Origin returns an opaque origin — the
     string "null" through the API — for a scheme that is not special, so `fetch("data:text/plain,x")` would
     have been filed at the address `nulltext/plain,x`, which is a plausible string and no address at all.
     Unmasking the parser's own href instead is the exact inverse of what was masked, so every address this
     file resolved correctly before the mask resolves byte-identically after it. */
  path = _astUnmaskHoles(_decHoles(url.pathname));
  return { originKnown: true, url: url, href: _astUnmaskHoles(_decHoles(url.href)),
           host: url.hostname, path: path };
}
