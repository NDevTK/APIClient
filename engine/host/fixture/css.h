/* THE CSS DOCUMENT: author sheets whose CSSOM answers each row reads, with no state and no fork.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CSS_H
#define ENGINE_HOST_FIXTURE_CSS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CSS[] =
    "<!doctype html><html><head>"
    /* The rule `HTML_PLATFORM`'s first sheet carries, under which these trees were written: the rows ask the cascade
       and CSSOM, and a displayed `.nstmedia` grid box is laid out by the selftests' paint, where css-grid's
       intrinsic sizes are unbuilt (core/layout/intrinsic_size.c says so by name). */
    "<style>div { display: none }</style>"
    /* CSS Properties and Values API 1 §3's `@property`, in a SHEET OF ITS OWN (found by its id) so its
       rules sit at fixed indices. Five bodies, because §3 decides five different things about
       one: every descriptor declared; NONE, which is what exercises §3.1's initial `"*"`, §3.2's initial `true`
       and §3.3's initial (the guaranteed-invalid value, i.e. a null `initialValue`); only an UNKNOWN
       descriptor, which §3 says is "invalid and ignored, but do not invalidate the @property rule" — so the
       rule must still be in `cssRules`; a `syntax` whose string §5.4.3 refuses (`inherit` is not a
       `<custom-ident>`), which §3.1 says leaves the descriptor ignored and therefore the initial standing; and
       §3.3's own cross-descriptor condition, which is the last one below and is the shape a shipping site
       actually declares — developer.mozilla.org ships
       `@property --switch-position{syntax:"<percentage>";inherits:false;initial-value:0}`, and it is the rule
       that used to abort the whole document at stage `create` with zero flows run. */
    "<style id=propsheet>"
    "@property --pfull { syntax: \"<percentage>\"; inherits: false; initial-value: 0% }"
    "@property --pbare { }"
    "@property --podd { not-a-descriptor: 3 }"
    "@property --pbad { syntax: \"inherit\" }"
    /* §3.3's CROSS-DESCRIPTOR CONDITION, which is the one thing about this interface a page can only see
       through `initialValue`: `if specified, the value of the initial-value descriptor must successfully parse
       according to the rule's syntax descriptor, or else the descriptor is invalid and ignored`. `0` is a
       `<number-token>` and CSS Values §5.5 makes a `<percentage>` a `<percentage-token>`, so the descriptor is
       ignored and §3.3's initial — the guaranteed-invalid value — stands. THIS IS THE MDN RULE EXACTLY:
       developer.mozilla.org ships `@property --switch-position{syntax:"<percentage>";inherits:false;
       initial-value:0}`, and the rule above (`--pfull`) is the same declaration written with the `%` that
       makes it parse — so the two differ by one byte and must differ in `initialValue`. */
    "@property --pmiss { syntax: \"<percentage>\"; inherits: false; initial-value: 0 }"
    "</style>"
    "</head><body>"
    /* ---- CSS Nesting Module Level 1 §3 "Nesting Style Rules" .. §6 "CSSOM" — the sheet, its tree, three claims.
       THE PARENT IS A SELECTOR LIST WHOSE MEMBERS DISAGREE ABOUT SPECIFICITY. CSS Nesting §4 "Nesting Selector:
       the & selector": "The specificity of the nesting selector is equal to the largest specificity among the
       complex selectors in the parent style rule's selector list (identical to the behavior of :is())". The host
       matches through `.nstcls`, never `#nstid`, so `& .nstkid` carries (1,1,0) where a FLATTENED `.nstcls
       .nstkid` carries (0,2,0) — and `.nstwrap .nstkid`, (0,2,0) written later, wins under the flattening and
       loses under the standard (§4's own worked example has this shape).
       ALL THREE §3.1 "Syntax" PRELUDE SHAPES ARE PRESENT, because each reaches this engine differently: the
       implied descendant (`.nstkid`, which the selector parser accepts and which is still relative), the
       nest-containing compound (`&.nstself`) and the leading combinator (`> .nstchild`), the last two refused
       by the selector parser. §6 requires all three to serialize ABSOLUTIZED — the second claim.
       The third is §3.3 "Nesting Other At-Rules", a nested group rule whose style rules take their nesting
       selector "from the nearest ancestor style rule"; it is its own endpoint because a nested `@media` that
       never becomes a rule is a separate capability from the cascade. */
    "<style id=nstsheet>"
    "#nstid, .nstcls { color: rgb(9, 9, 9);"
    "  .nstkid { display: block }"
    "  &.nstself { display: inline-block }"
    "  > .nstchild { display: flex }"
    "  .nstdeep { .nstdeeper { display: table } }"
    "  @media all { .nstmedia { display: grid } }"
    "}"
    ".nstwrap .nstkid { display: list-item }"
    "</style>"
    "<div class=nstwrap><div id=nsthost class=\"nstcls nstself\">"
    "<span id=nstkid class=nstkid></span><span id=nstchild class=nstchild></span>"
    "<span id=nstmedia class=nstmedia></span>"
    "<span class=nstdeep><span id=nstdeeper class=nstdeeper></span></span>"
    "</div></div>"
    "<script>var nsh = document.getElementById('nstsheet').sheet;"
    "var nsr = nsh.cssRules[0]; var nsc = nsr.cssRules;"
    "var nsel = nsc.length + '|' + nsc[0].selectorText + '|' + nsc[1].selectorText + '|' +"
    " nsc[2].selectorText + '|' + nsc[3].selectorText + '|' + nsc[3].cssRules[0].selectorText;"
    "fetch('/api/cssnestsel?v=' + (nsel === '5|& .nstkid|&.nstself|& > .nstchild|& .nstdeep|& .nstdeeper'"
    " ? 'NESTSELOK' : 'NESTSELBAD:' + nsel));"
    "var nsd = function(i){ return getComputedStyle(document.getElementById(i)).display; };"
    "var nsv = nsd('nstkid') + '|' + nsd('nsthost') + '|' + nsd('nstchild') + '|' + nsd('nstdeeper');"
    "fetch('/api/cssnest?v=' + (nsv === 'block|inline-block|flex|table' ? 'NESTOK' : 'NESTBAD:' + nsv));"
    "fetch('/api/cssnestmedia?v=' + (nsd('nstmedia') === 'grid' ? 'NESTMEDIAOK' : 'NESTMEDIABAD:'"
    " + nsd('nstmedia')));</script>"
    /* ---- CSS Compatibility Standard §3.1 "CSS At-rules": the `-webkit-` prefixed at-rules "must be supported as
       aliases of the corresponding unprefixed at-rules" — a NAME RESOLUTION: the same interface, `type`,
       `<keyframes-name>` grammar and body, and the one thing that differs.
       SEVEN AT-RULES, EXACTLY TWO CSSOM RULES. `spin` is the alias and `spin2` the unprefixed rule beside it,
       so serialization is a comparison. `@-moz-keyframes` and `@-ms-viewport` are another vendor's extensions
       (CSS 2.1 §4.1.2.1 "Vendor-specific extensions" reserves them, §4.2 "Rules for handling parsing errors"
       ignores them); `@-webkit-keyframes none` meets §3's `<keyframes-name>` exclusion, so the alias resolves
       BEFORE the prelude is parsed. `@three-dee` (CSS 2.1 §4.2's own example, with its nested
       `@background-lighting`) and `@medium` are unprefixed at-keywords no specification defines, judged by CSS
       Syntax Level 3 §8 "CSS stylesheets": "any at-rule [that] is not recognized … discard that rule". Both
       bodies hold a rule that would be a CSSRule at top level, so a build that drops the head and keeps the
       contents moves the LENGTH, which the token's first field carries.
       THE EXPECTED STRINGS ARE REAL CHROME 148.0.7778.167's for this sheet: CSSOM §6.4's CSSKeyframesRule arm
       names the literal `"@keyframes "` and predates §3.1, so the serialization half is a measurement. */
    "<style id=kfsheet>"
    "@-webkit-keyframes spin { from { opacity: 0 } to { opacity: 1 } }"
    "@keyframes spin2 { 0%, 50% { opacity: 0 } }"
    "@-moz-keyframes spin3 { from { opacity: 0 } }"
    "@-webkit-keyframes none { from { opacity: 0 } }"
    "@-ms-viewport { width: device-width }"
    "@three-dee { @background-lighting { azimuth: 30deg; elevation: 190deg; } h1 { color: red } }"
    "@medium { .cs-bp { height: auto } }"
    "</style>"
    /* TWO ROWS BECAUSE THESE ARE TWO INDEPENDENT CLAIMS, the same arrangement `/api/cssprop` above takes: what
       the alias PRODUCES (one CSSKeyframesRule, §6.4.2's `type` 7, §6.3.2's `name`, its `<keyframe-block>`s
       canonicalised by §6.2.2 — `from` reads back as `0%`) and what §6.4 SERIALIZES. A build could get the
       object right and emit `@keyframes` for it, and one boolean over both would not say which. */
    "<script>var kfr = document.getElementById('kfsheet').sheet.cssRules;"
    "var kv = kfr.length + '|' + (kfr[0] instanceof CSSKeyframesRule) + '|' + kfr[0].type + '|' + kfr[0].name"
    " + '|' + kfr[0].cssRules.length + '|' + kfr[0].cssRules[0].keyText + '|' + kfr[1].name;"
    "fetch('/api/csswkkf?v=' + (kv === '2|true|7|spin|2|0%|spin2' ? 'WKKFOK' : 'WKKFBAD:' + kv));"
    "var kt = kfr[0].cssText + '~' + kfr[1].cssText;"
    "fetch('/api/csswkkfText?v=' + (kt === '@-webkit-keyframes spin { \\n  0% { opacity: 0; }\\n"
    "  100% { opacity: 1; }\\n}~@keyframes spin2 { \\n  0%, 50% { opacity: 0; }\\n}'"
    " ? 'WKKFTEXTOK' : 'WKKFTEXTBAD:' + kt));</script>"
    /* §6.1's FOUR ATTRIBUTES AND ITS SERIALIZATION, READ FROM THE PAGE. They are IDL getters, so the read is
       page code on the trampolined chain (a C activation has no flow base, and `JS_GetPropertyInternal`
       refuses to run a getter from one); `document.styleSheets`, `cssRules`, the indexed getter and the rule
       objects are the whole route, so a member missing from §6.1's prototype fails here.
       ONLY A TOKEN CROSSES THE URL: the query percent-encode set encodes space, `"`, `<` and `>`, so a syntax
       string shipped as a value is an assertion about the encoder. The expected bytes are JS literals, and the
       failing arm carries the actual value.
       TWO FETCHES, TWO CLAIMS — what the attributes ANSWER and what §6.1's serialization EMITS.
       NO FORK: every value is concrete. The joined string leads with the rule COUNT, because §3 says neither
       an unknown descriptor nor an unreadable syntax invalidates the rule (all five are in `cssRules`), and it
       ends with the two syntaxes that reach §3.1's initial by two routes. `type` is 0 because §6.4.2's table
       is frozen and this interface is not in it. */
    "<script>var pss = document.getElementById('propsheet').sheet.cssRules;"
    "var pv = pss.length + '|' + pss[0].name + '|' + pss[0].syntax + '|' + pss[0].inherits + '|' +"
    " pss[0].initialValue + '|' + pss[0].type + '|' + pss[1].syntax + '|' + pss[1].inherits + '|' +"
    " pss[1].initialValue + '|' + pss[2].syntax + '|' + pss[3].syntax + '|' + pss[4].initialValue;"
    "fetch('/api/cssprop?v=' + (pv === '5|--pfull|<percentage>|false|0%|0|*|true|null|*|*|null'"
    " ? 'CSSPROPOK' : 'CSSPROPBAD:' + pv));"
    "var pt = pss[0].cssText + '~' + pss[1].cssText;"
    "fetch('/api/csspropText?v=' + (pt === '@property --pfull { syntax: \"<percentage>\"; inherits: false;"
    " initial-value: 0%; }~@property --pbare { syntax: \"*\"; inherits: true; }'"
    " ? 'CSSTEXTOK' : 'CSSTEXTBAD:' + pt));</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
