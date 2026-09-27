/* WHAT A PLAIN PARSE OF A REAL BUNDLE RECOVERS, SO THAT WHAT EXECUTION ADDS CAN BE PRICED.
 *
 * THE QUESTION THIS EXISTS TO ANSWER IS STRATEGIC AND WAS ASKED BY THE PROJECT OWNER: does forced execution
 * still learn anything on a large real web app that a plain Babel parse of the same bundle would not? Most of
 * this engine is a browser, and a browser is expensive; if a parser recovers the same surface, the cost buys
 * nothing on the @H half and has to be justified on VALUES and on the @S half or not at all.
 *
 * IT IS A CONTROL AND NOT A TARGET, AND THAT DISTINCTION IS LOAD-BEARING RATHER THAN A DISCLAIMER. CLAUDE.md
 * §What-the-tool-produces says `netdiff --unused` is "a DIAGNOSTIC that the solver dominates the live page,
 * NOT the optimization target"; the same sentence governs this file. A number here going UP is not progress
 * and a number here going DOWN is not progress — the only thing either says is how much of the surface a
 * cheaper instrument already reaches. Optimising the engine toward this baseline would be optimising toward
 * a parser, which is the one thing the engine is not.
 *
 * WHAT IT DOES NOT MEASURE, STATED FIRST BECAUSE A COVERAGE FIGURE THAT DOES NOT NAME ITS DENOMINATOR IS THE
 * DEFECT CLAUDE.md §a-coverage-figure-states-what-it-is-a-fraction-of IS ABOUT. This reads the JS DOOR only:
 * the addresses a bundle's own CODE names. The engine has a second door — markup — and `git grep -c
 * endpoint_record` puts EIGHT of its recording sites in `html_link.c`, three in `html_image.c`, one each in
 * `html_script.c` and `html_form.c`, against one each in `fetch.c`, `xml_http_request.c`,
 * `navigator_beacon.c`, `multipart_batch.c`, `reply_decode.c` and `engine.c`. A `<script src>` and a `<link
 * href>` are recovered COMPLETELY by any HTML parser and by the engine alike, so counting them would add one
 * number to both sides of the comparison and settle nothing. They are excluded, they are excluded on
 * purpose, and the excluded population is reported beside the included one rather than left to be inferred.
 *
 * THE POPULATION COMES FROM `engine/corpus_programs.mjs` AND NOT FROM A FILENAME, for the reason that file's
 * own header gives at length: a fetcher folded a URL's query into the saved name, so three genuine shipped
 * bundles had extensions no list would carry, and a corpus that is quietly smaller reports a smaller number
 * in the flattering direction. That module joins bytes to the server's own `Content-Type` by sha256 and
 * THROWS on any file it cannot type. THIS FILE REPRODUCES ITS PUBLISHED TOTALS BEFORE PRINTING ANY BREAKDOWN
 * OF THEM, which is the calibration CLAUDE.md §AND-WHERE-THE-SUBJECT-ALREADY-PUBLISHES-A-TOTAL prescribes:
 * a probe that is a second implementation of somebody's selector can walk what the tree DECLARES where the
 * instrument walks what it INSTALLS, and the disagreement is invisible in its output.
 *
 * THE DOORS ARE READ OUT OF THE ENGINE AND NOT INVENTED HERE. A list of "things that look like requests"
 * would be a second copy of a fact `endpoint_record`'s callers already state, and the copy anyone writes
 * first is the one that drops a door. Each entry in `DOORS` below names the engine file whose
 * `endpoint_record` call it mirrors, so the two can be diffed by anyone; a door with no engine site is
 * marked as such and is a FLOOR-WIDENING rather than a comparison.
 *
 * IT IS A PARSE AND NEVER A REGEX OVER SOURCE TEXT. CLAUDE.md §RUN-DON'T-MATCH bans the second, and it bans
 * it in BOTH directions here: a regex baseline would be artificially weak, which would make the engine look
 * artificially good, which is the one result this file must not manufacture. `@babel/parser` is the parser
 * the question was asked about by name. MEASURED at the corpus this ran against: it parses 721 of 724
 * non-`index` files and the three refusals are HTML documents, so the parse is not the limiting factor on
 * the static side and cannot be offered as an excuse for a low number.
 *
 * THE KIND PARTITION IS THE WHOLE ANSWER AND A SINGLE COUNT WOULD DECIDE THE QUESTION WRONGLY. A parser and
 * an interpreter differ on exactly one axis — whether the URL's value is in the TEXT or only in a RUN — so
 * the four kinds below are not a presentation choice:
 *   LITERAL  the argument is one string literal. A parse has it; so does the engine; execution adds nothing.
 *   FOLDED   the argument is a computation over bindings this file could resolve WITHOUT RUNNING ANYTHING.
 *            A parse has it too, which is the half that makes the baseline fair rather than a strawman.
 *   SHAPE    it resolves to literal text plus at least one hole — `"/api/" + region`. A parse gets the
 *            SHAPE and can never get the VALUE; the engine can, because it ran the concatenation on a real
 *            operand. THIS ROW IS THE ENTIRE ARGUMENT FOR EXECUTION ON THE @H HALF.
 *   OPAQUE   it resolves to no literal text at all — a bare identifier, a member, a call. A parse knows
 *            only that a request happens here. Same reading as SHAPE and stronger.
 * A high LITERAL+FOLDED share is a finding AGAINST the browser on paths and must be reported as one.
 *
 * AND A SECOND AXIS, BECAUSE THE FOUR KINDS ABOVE ARE ABOUT THE ADDRESS AND SAY NOTHING ABOUT WHETHER A RUN
 * EVER REACHES THE CALL. The kinds answer "could a parse have had this value"; the REACH band answers "what
 * has to be CALLED for this line to run", which is the only axis on which a door a run rings and a door it
 * does not come apart. A parse reads a call whether or not anything invokes its enclosing function, so this
 * band costs the parse nothing and is not a concession — it is what makes a door's ZERO IN A RUN readable,
 * which no column here could do before. Three arms, an exact partition of each class's own `sites`:
 *   TOP-LEVEL   function depth 0 — the module or script body performs the call, so evaluating the program
 *               reaches it and nothing else has to happen.
 *   ASYNC FN    the INNERMOST enclosing function is `async`. It runs only once something invokes that body,
 *               and in a real application the invoker is an effect flushed after a render commit, an event
 *               handler, a `setTimeout` or an idle callback — none of which is program evaluation.
 *   SYNC FN     the innermost enclosing function is an ordinary one. It also needs an invoker.
 * THE PARTITION IS ASSERTED against `sites` for both classes, so no row can fall out of all three and make a
 * class read as having fewer async sites than it has — the flattering direction for the DATA door, whose zero
 * in a run is the thing this band exists to make readable.
 * IT IS A FLOOR IN ONE DIRECTION ONLY AND THE ASYMMETRY IS THE WHOLE OF HOW TO READ IT. `innerSync`
 * OVER-states reachability, because a sync function nothing calls is exactly as unreached as an async one;
 * `innerAsync` cannot over-state it, because an async body needs an invoker by construction. So a high
 * `innerAsync` share IS evidence a class needs an invoker, and a high `innerSync` share is NOT evidence that
 * it does not. Reading the second as a clean bill is the one reading this band must not be used for.
 * WHAT IT DOES NOT ANSWER, STATED HERE BECAUSE THE BAND IS ONE HOP SHORT OF THE QUESTION A READER WANTS: it
 * says a call needs an invoker and never WHETHER THAT INVOKER IS ITSELF REACHED. A sync function called from
 * top level is reached and a sync function called only from an async one is not, and both land in `innerSync`.
 * The call graph is what separates them, this file builds none, and a scope-resolved fold is not one — so the
 * band is a NECESSARY-CONDITION reading and never a sufficient one. HOW ITS ABSENCE WOULD SHOW: a corpus
 * whose DATA door is almost all `innerSync` would read as needing no invoker while every one of those
 * functions sat behind an async caller. WHAT THE NEXT DIFF BUILDS: a reachability closure over the call graph
 * this file can already name — the function declarations its scope pre-pass resolves — so `innerSync` splits
 * into "called from a body the program evaluates" and "called only from somewhere that itself needs an
 * invoker", which is the same fold already built for the chunk manifest pointed at callers instead of at
 * addresses.
 *
 * FOLDING IS DELIBERATELY CONSERVATIVE AND THE NUMBER IS THEREFORE A FLOOR FOR THE PARSE, WHICH IS THE
 * DIRECTION THAT COSTS THIS PROJECT RATHER THAN FLATTERS IT. An identifier is folded only where the BINDING
 * IT RESOLVES TO is declared once in its own scope and written never, so no shadowing can make a fold wrong
 * — a reference names exactly one binding and that is the one folded. A real commercial extractor does
 * interprocedural constant propagation and would fold MORE. Reporting a floor for the side whose strength is
 * inconvenient is the only honest direction: CLAUDE.md §A-SWEEP-IS-TRUSTED-BY-ITS-METHOD says a static
 * derivation over text is a lower bound wearing a total's clothes, and here the lower bound belongs to the
 * baseline rather than to the subject.
 * THIS SENTENCE USED TO SAY `bound EXACTLY ONCE in the whole file`, AND THE ARGUMENT IS KEPT AT
 * `collectBinds` RATHER THAN HERE BECAUSE THAT IS WHERE A READER WILL RE-DERIVE IT. What the change buys is
 * measured and not asserted: on the corpus this file was last run against, the DATA door's complete-from-text
 * share moved 11.9% -> 15.1% and the chunk manifest's recovered address set 758 -> 2008, because a minified
 * runtime binds one letter in a dozen nested functions that enclose neither its declarations nor its uses.
 * Every one of those is an address a competent static tool states and this file was crediting to execution.
 *
 * A ZERO IN ANY ROW IS READ AGAINST THE BASE RATE PRINTED BESIDE IT. `pathish` counts distinct string
 * literals in the same programs that LOOK like addresses and are attached to no door — the population a
 * naive "grep the bundle for /api/" tool reports. It is NOT an endpoint count and must never be quoted as
 * one; it is there so that "the parse found N request sites" can be read against "and M address-shaped
 * strings it could not attach to any request", which is what finding something would have looked like.
 *
 * THE CORPUS IS NOT TRACKED AND THE DRIVER IS, which is `testing/corpus/README.md`'s split and not this
 * file's choice: this repository carries no copy of anybody else's site. So every figure printed here is a
 * fact about ONE FETCH, at the instant that fetch's own manifest names, and the instant is printed with the
 * numbers. Without a corpus this file THROWS and the throw carries the command that makes one — a throw that
 * names a hazard and offers no exit is the shape CLAUDE.md
 * §A-CONTRACT-THAT-NAMES-A-HAZARD-AND-OFFERS-NO-EXIT forbids.
 *
 * THE DECLARED BLIND SPOTS CARRY A SIZE AND NOT A SENTENCE, because a floor that names what it excludes
 * without measuring it is read as a total anyway:
 *   - `el.src = url` / `el.href = url` — the door `html_script.c` and `html_link.c` DO record and no door
 *     above reads, because `.src` is a property of many things that are not elements and admitting it to
 *     the door set would buy recall with precision this comparison cannot afford. THAT EXCLUSION STANDS
 *     AND ITS SIZE IS NOW COUNTED ON EVERY RUN, per site, in the same four kinds, printed as THE DOOR
 *     SET'S OWN BLIND SPOT and summed into no door total. The count is an OVER-count of elements by
 *     construction — every `.src`/`.href` assignment in the file, element or not — which is the safe
 *     direction for the size of a blind spot: a blind spot stated too large certifies nothing, while one
 *     stated too small is read as a clean bill.
 *     THIS PARAGRAPH USED TO CARRY THE SIZE AS FOUR FROZEN NUMBERS — "217 `.src =` and 212 `.href =`
 *     assignments in the corpus and 21 and 8 of them have a single string literal on the right — so 400
 *     of 429 are computed" — and it is rewritten rather than deleted because the ARGUMENT is right and a
 *     reader who re-derives it will re-add the figures. A count over a corpus this repository does not
 *     carry is unreproducible BY CONSTRUCTION, so it cannot be checked and cannot go loudly wrong: a
 *     re-derivation at a later fetch answered 218 and 215 against its 217 and 212. The claim that a reader
 *     widening this file "should expect to add mostly OPAQUE rows" is the part that HELD and is what the
 *     band now measures rather than asserts.
 *     THE REASON THE BAND EXISTS RATHER THAN A WIDER DOOR SET IS A MEASUREMENT AND NOT A PREFERENCE. The
 *     PROGRAM door is BIMODAL BY BUNDLER: a bundle that ships native `import()` scores in the door, and
 *     one whose bundler compiled `import()` away into a chunk-id map plus a `<script>` injection scores
 *     ZERO there. The address is then composed through a CALL (`script.src = R.tu(R.p + R.u(id))`), so a
 *     `.src` door would add one OPAQUE row per runtime and recover NO address — recall bought for nothing,
 *     and precision spent. What recovers those addresses is interprocedural folding through the chunk-URL
 *     function, which is a different subproblem and is the CHUNK MANIFEST channel — built, and reported as
 *     its own band rather than as a door, for the same reason this exclusion stands.
 *   - a library wrapper (`axios.get`, `$.ajax`, an SDK `request()`), for the reason the DOORS table gives.
 *   - anything a bundle reaches through a member call this file cannot name, which is unbounded and is why
 *     the site count here is stated as a floor everywhere it is stated at all.
 *
 * THE CHUNK-MANIFEST RESIDUAL IS BUILT AND WHAT REPLACES IT IS NARROWER AND NAMES A DIFFERENT MECHANISM.
 * It asked for a fold that resolves a PROPERTY assigned exactly once in the file, inlines a single-parameter
 * function, and ENUMERATES a ternary chain or a computed member into the set of addresses it can return.
 * That is what the CHUNK MANIFEST channel is, it is keyed on the expression and on no runtime's name, and
 * the two forms it was written from both answer: a ternary chain over a public-path literal, and two object
 * literals joined at one key. It found a PRECISION half its own clause did not anticipate and that is worth
 * keeping, because the clause would otherwise be read as finished: the shape it describes is also the shape
 * of an i18n table, an enum and a label map, so before an address test was added the channel reported
 * `session`, `Users` and `0.001` as addresses and its figure was roughly double.
 * THAT RESIDUAL'S OWN SUCCESSOR — THE SCOPE PRE-PASS — IS BUILT, AND ITS REMEDY CLAUSE HELD, WHICH IS WORTH
 * SAYING BECAUSE CLAUDE.md RATES A NEXT-DIFF CLAUSE AS THE HALF THAT USUALLY YIELDS. It asked for one scope
 * pre-pass producing the BINDER of every Identifier by node identity, so the slot map and the
 * function-declaration lookup key on the binding rather than on the name, and said it would be strictly both
 * more precise and wider than the file-wide count it replaced. It is, and `collectBinds` holds the argument.
 * Its HOW-ITS-ABSENCE-WOULD-SHOW clause was the reliable one again: it named a site whose PROGRAM-door row
 * reads 0, whose blind-spot OPAQUE count beside it is nonzero, and whose manifest column reads 0 — and on the
 * corpus this was last run against, the webpack site that read that way went from 0 recovered addresses to
 * the whole image of its runtime's chunk-URL function. ONE site still reads that way and its cause is a
 * DIFFERENT mechanism, established rather than guessed: it is a rollup bundle whose `.src` writes are
 * third-party script injections (a consent SDK, a CDN snippet) and which emits no chunk-id manifest at all,
 * so there is nothing there for this channel to recover and its zero is correct.
 * WHAT IS NOT COVERED NOW, AND BOTH HALVES CARRY THE COMMAND THAT SIZED THEM RATHER THAN A SENTENCE.
 * (a) A COMPOSITION HOLDING SEVERAL APPLICATIONS OF THE SAME ARGUMENT. The scanner refuses any composition
 * with more than one application, because two unknown parameters make the address set a product of two
 * domains — which is right in general and stricter than the hazard when every application is handed the SAME
 * argument expression, since binding ONE candidate and folding ALL of them keeps the applications correlated
 * exactly as folding two maps at one candidate already does. WHAT THE NEXT DIFF BUILDS: `env.app` becomes a
 * SET, `foldEnvOnly`'s call arm inlines any member of it, and the scanner admits a multi-application
 * composition only where the argument expressions are identical. HOW ITS ABSENCE WOULD SHOW: the refusal
 * count printed under the manifest band stays the whole population rather than the uncorrelated remainder of
 * it. The size is the reason this is a residual and not this diff: of the refusals on the corpus this was
 * last run against, THIRTEEN shared one argument expression and 182 did not — derive it by counting, at the
 * refusal, the compositions whose applications' single arguments are the same Identifier.
 * (b) A COMPOSITION THAT RECOVERS A CHUNK NAME WITHOUT ITS PUBLIC PATH. Those are counted as FRAGMENTS and
 * refused, and the count is LARGE — which reads as a recall hole and measurably is not. Dumping the refused
 * texts, the population is i18n tables, enum labels and error strings (`height`, `Service accounts`, a
 * framework's rate-limit warning), which is the precision test doing exactly the job the paragraph above
 * describes; only a small minority are shaped like a bare chunk filename. WHAT THE NEXT DIFF BUILDS: nothing
 * yet, because the fragment count is evidence the shape test is working and a widening here would have to
 * establish which enclosing expression supplies the public path before it could join one. HOW ITS ABSENCE
 * WOULD SHOW: a site whose manifest FRAGMENT count is nonzero while the fragments themselves are chunk
 * filenames rather than labels — which is what the refused texts have to be read for, and is why this half
 * is stated as a shape rather than as a number.
 *
 * THE OCCLUDING LEAF IS GONE AND WHAT REPLACES IT IS TWO RESIDUALS, EACH WITH A MEASURED SIZE AND NEITHER
 * NAMING A SITE. `new URL(...)` was a LEAF of this fold, so the composition inside it was never folded at
 * all — CLAUDE.md §AND-A-FINDING-CAN-OCCLUDE-ITS-OWN-SUCCESSOR's shape, where the outer node is one row AND
 * removes the only route to what is under it. It is folded now, by the URL Standard's own resolution applied
 * to two recovered strings, together with `String(x)`, `x.toString()`, `new Request(x)` and an
 * INTERPROCEDURAL INLINE that crosses into a callee with the argument its call site supplies. What the
 * widening COST is printed on every run beside what it bought, for the reason the global door's three
 * numbers are: a widening whose refusals are unmeasured is a trade nobody made.
 * (a) WAS "A CALLEE THAT REACHES ITS OBJECT AS A PARAMETER", AND IT IS BUILT — the unique-call-site closure
 *     is in `collectBinds` and its price prints on every run. THE MECHANISM IT NAMED IS SOUND AND ITS
 *     DIAGNOSIS WAS WRONG, and the diagnosis is recorded here because a reader who re-derives it will draw
 *     the same conclusion: the clause said the obstacle was a MEMBER callee whose object is a parameter, and
 *     pricing the built closure says the obstacle is that THE FUNCTION OWNING THE PARAMETER HAS NO NAME AT
 *     ALL. Measured over the refused population, that half is a CLASS METHOD or an anonymous function or
 *     arrow — reached through a RECEIVER or by being PASSED — and NOT the bundler's module shape this clause
 *     first guessed at. THAT CORRECTION IS NOT THE END OF IT, BECAUSE THE SENTENCE THAT REPLACED IT WAS AN
 *     UNDER-CLAIM: it said no refinement of the reference count reaches that half, which is true of a
 *     reference COUNT and reads as "nothing reaches it", and a reader who believes it does not look. The
 *     refusal block now partitions it BY SYNTACTIC ROLE and the three populations take OPPOSITE work:
 *       · THE VALUE OF A MEMBER WRITE — `o.f = u => fetch(u)` — is REACHABLE, by the same once-written
 *         question `slotOf` already answers for a property, asked of the SLOT instead of the binding. It is
 *         the largest single member of the half and it is a queue rather than a wall.
 *       · A CLASS or OBJECT METHOD is reached through a RECEIVER, so naming its arguments needs the
 *         receiver's TYPE — a recogniser §RUN-DON'T-MATCH forbids, whatever it is called.
 *       · A CALLBACK ARGUMENT has its parameter bound by the CALLEE'S OWN SEMANTICS: a resolution value, an
 *         array element, an Event. There is no argument in this file to read, so no parse recovers it at any
 *         strength, and its count is not a queue for anything on this side of the comparison.
 *     Read the reasons apart; one number over the three says none of that.
 *     WHAT THE CLOSURE ITSELF BOUGHT, and it is the honest half: it settled
 *     ZERO COMPLETE addresses over this corpus and moved three rows out of `opaque` into `shape`. That does
 *     NOT move the razor, because SHAPE and OPAQUE are both inside it — what it moved is this file's shape
 *     RECALL, which is a real gain in what a static reader is handed and not a gain against the engine.
 * (a'') THE VALUE OF A MEMBER WRITE, WHICH IS THE ONLY REACHABLE THIRD OF (a)'s REFUSED HALF. NOT COVERED:
 *     the closure asks whether a function's BINDING is referenced once, and a function assigned to a
 *     PROPERTY has no binding at all, so `o.f = u => fetch(u)` with one `o.f(x)` read is refused even though
 *     the slot names its one entry as plainly as a binding would. WHAT THE NEXT DIFF BUILDS: the same
 *     admitting rule keyed on the SLOT — a function that is the single plain `=` write to `obj.prop` where
 *     `obj` resolves to a binding declared once and never written and nothing writes that object through a
 *     computed property, entered by the one READ of that slot standing in callee position. Every one of those
 *     conditions already exists in `collectBinds` as `slotWrites`, `slotPoison` and `slotKey`; what is new is
 *     counting the slot's READS and requiring exactly one in callee position. HOW ITS ABSENCE WOULD SHOW: the
 *     closure's refusal block carries a nonzero row naming the value of a member write, while its settled
 *     count does not move.
 *     AND ITS EXPECTED YIELD IS ZERO COMPLETE ADDRESSES, WHICH IS THE MEASUREMENT AND NOT A PREDICTION FROM
 *     TASTE: of the crossings the closure already makes, EVERY ONE landed on an argument carrying a hole, and
 *     all but three carried no literal byte at all. The boundary is not where the addresses are being lost —
 *     the caller had not spelled them either. Widening the reach does not change that ratio, so (a'') buys
 *     recall and not razor, and anyone building it should say so before they start.
 * (a') A REFERENCE THAT IS A CALLEE ONLY THROUGH `.call` OR `.apply`. NOT COVERED: the closure admits a
 *     reference standing in callee position and nothing else, so `f.call(recv, x)` and `f.apply(recv, [x])`
 *     put that one reference in an OBJECT position and the function is refused as un-entered — correctly by
 *     the closure's own rule and needlessly, because those two spellings name the argument list as plainly as
 *     a direct call does. WHAT THE NEXT DIFF BUILDS: two more admitting shapes at the same soundness bar —
 *     a sole reference that is the object of a non-computed `.call` member whose own parent is a call, with
 *     parameter i taken from argument i+1; and the same for `.apply` where the second argument is an
 *     ArrayExpression carrying no spread, with parameter i taken from element i. Both refuse a computed
 *     property and a further-wrapped receiver, for the reason the closure refuses a spread. HOW ITS ABSENCE
 *     WOULD SHOW: the closure's refusal block carries a nonzero row reading that a function's one reference
 *     is not a callee, while its settled count does not move.
 * (b) WAS "A BOUND `URL` THAT NOTHING MUTATES", AND IT IS MEASURED AND DECLINED. `deref` demotes a
 *     constructed URL read out of a name, a slot or an object-literal property, because a URL is mutable and
 *     a `searchParams` write leaves no trace in the text this fold reads; that is a FLOOR and the clause
 *     proposed proving the absence of mutation so the hole could drop. THE CLAUSE WAS TOO NARROW AS WRITTEN
 *     and the correction is the part worth keeping: "no member call and no member write on the binding" does
 *     not bound mutation at all — a URL ESCAPES through `f(r)`, an alias, an array or object it is put into,
 *     a return and a closure capture, none of which is a member call or a write; `r.searchParams` is a member
 *     READ that hands out a mutator; and a blanket "no member expression on `r`" would refuse
 *     `fetch(r.toString())`, which is the dominant shape the floor exists to serve. A sound closure is
 *     therefore a per-property classification from the URL Standard PLUS an escape analysis enumerating the
 *     reference roles that are safe and refusing everything unlisted.
 *     IT IS NOT BUILT BECAUSE THE CEILING IS PRINTED AND THE TRADE IS THE WRONG WAY ROUND. The mutable-fold
 *     floor's own block names every demoted row with its coordinates, so the most the escape analysis could
 *     ever recover is that count — and against it stands the failure it would introduce, which is a false
 *     COMPLETE: this file's one measured false resolution was exactly that, a URL reported as a whole address
 *     whose real request carries five query parameters the text does not hold. A gap understates the parse and
 *     a false complete OVERSTATES it, and only one of those two is the direction this file is required to be
 *     wrong in. BUILD IT WHEN the floor's block names rows that reading the bundle shows are NOT mutated —
 *     which is a question the printed coordinates answer and this sentence cannot.
 * (c) A PROMISE RESOLUTION VALUE IS NOT A REPLY, AND THE BAND DOES NOT DISTINGUISH THEM. NOT COVERED: the
 *     `WHY` band classifies a parameter bound by `.then`/`.catch`/`.finally` as A PROMISE RESOLUTION VALUE
 *     without asking what its receiver settles with, so a field out of a fetched body and a value out of an
 *     app-internal promise land in ONE bucket — and the first is precisely the case §Learning-from-replies
 *     calls the point of the tool, the value the engine has and no parse ever will. WHAT THE NEXT DIFF
 *     BUILDS: trace the receiver, and where it is a call whose callee is a DATA-class door, or a member call
 *     on such a call — `fetch(x).then(r => r.json()).then(f)` — the class becomes a reply body. The door map
 *     is already the one place that says which callees are data doors, so this READS it rather than restating
 *     it, and `collectBinds` stays free of any notion of a door: the spelling is recorded there and every
 *     classification stays in `VALUE_SUPPLIERS`'s own layer. HOW ITS ABSENCE WOULD SHOW: the band's promise
 *     bucket stands above zero while no bucket names a reply, so a reader cannot tell which of those rows the
 *     engine's reply learning reaches and which are the page talking to itself.
 *     WHAT IT MAY NOT BE PRICED BY, and this is why it is a residual rather than a gap: it RESOLVES NOTHING.
 *     It splits one bucket into two. A reader who scores it by rows moved out of `opaque` will score it zero
 *     and conclude it is worthless, which is the same misreading the band as a whole invites — the worth of a
 *     partition is that two mechanisms can be told apart, and the honest measure is whether every row lands
 *     in exactly one class with none forced.
 * (d) A CALLEE'S OWN STRAIGHT-LINE WRITES, WHICH THE SECOND FIELD ON `a call result` MEASURED AND NAMED.
 *     NOT COVERED: the door channel's inline arm crosses the callee boundary only for a function whose WHOLE
 *     BODY IS ONE RETURNED EXPRESSION, so a body that assigns a local and returns it is refused and its call
 *     site stays a hole — and the census beside the band says what those callees do instead: of the calls
 *     whose callee it resolves to one definition in the same file, ZERO return text this fold can settle, and
 *     the rest return either an expression naming a PARAMETER or a LOCAL the body assembled. Reading three of
 *     the second kind in the mirror found each to be an alias of a parameter one write earlier (`let e=r; …
 *     return e`), so this is ONE mechanism and not two.
 *     WHAT THE NEXT DIFF BUILDS: widen `returnExprFnOf` past the one-statement rule for a body whose
 *     statements are a straight line — no branch, no loop, no `try` — by folding each plain assignment to a
 *     declared local in order into the env the return is folded with, refusing on any conditional write to a
 *     name the return reads, on `arguments`, and on an `async` or generator function exactly as that helper
 *     already does. The arity and argument tests stay the caller's, so a parameter the call site did not
 *     settle still holes; what changes is that the returned LOCAL stops being one.
 *     HOW ITS ABSENCE WOULD SHOW: the second field's block reports resolved callees whose return is not text
 *     while its credited count stands at zero, and the band goes on reporting those call sites as `a call
 *     result` — so a reader sees a definition named in this file's own output and no row anywhere saying what
 *     it returns.
 *     WHAT IT MAY NOT BE PRICED BY: a body that assigns and returns is a body whose value depends on the
 *     statements before the return, which is exactly the reason the one-statement rule exists — so this is a
 *     widening that must be paid for in FALSE COMPLETES and measured as one, built as a classifier and run
 *     over the whole population before it lands, with the rows it newly settles read at their coordinates.
 *
 * WHAT COMPLETES THE COMPARISON, NAMED SO IT CAN BE RUN RATHER THAN RE-DERIVED. This file is one half. The
 * other half is not "the engine's endpoint count", which answers a different question: `solver/result.c`
 * publishes `epEmitted` and `epPreProgram`, and its own comment says `epEmitted - epPreProgram` is "the most
 * addresses forced execution can have contributed to this document's surface, so a run reading them EQUAL
 * learned nothing the markup did not already state". THAT DIFFERENCE IS THE ENGINE SIDE OF THIS COMPARISON.
 * The measurement that closes it: drive a real app page through the WASM artifact with
 * `testing/harness.js restart` at a revision that publishes both rows, and read the difference.
 * IT CANNOT BE TAKEN FROM THE ARCHIVE TODAY, and the reason is a partition rather than an absence — the
 * archived rows that publish BOTH are ten, and they are two disjoint populations: the real-SPA rows are the
 * NATIVE `--abi` arm, where the reply door reads 43 asked / 1 answered and the surface is 43 = 43 (execution
 * contributed nothing), and the rows where execution contributed everything (516 = 516 - 0) are the build's
 * own SYNTHETIC fixture, whose `epPreProgram` is 0 because it has no markup door at all. No archived row is
 * a REAL page under the WASM artifact publishing `epPreProgram`. Under that artifact the same page's reply
 * door reads 43 asked / 43 answered with `rowsAwaitingBytes` 0 — so the bytes DO arrive there — and its
 * fork count is 3 against the native arm's 6242, which is why the missing row is worth taking rather than
 * predicted: one arm fetches and does not explore, the other explores and does not fetch.
 *
 * THIS FILE'S OWN NUMBERS ARE LOAD-INDEPENDENT AND THAT IS WHY THEY MAY BE QUOTED AT ALL. CLAUDE.md §Testing
 * forbids quoting a rate taken while a build loads the box, and everything here is a COUNT over bytes that
 * do not move: the same corpus gives the same answer on a busy machine and an idle one. The only figure that
 * is not is the parse time, which is printed as a fact about the run and is in no conclusion.
 *
 *   NODE_USE_ENV_PROXY=1 SITES=apps.tsv node testing/corpus/fetch.mjs   # make a corpus
 *   node testing/static_surface.mjs                                     # read one
 *   node testing/static_surface.mjs --json > out.json                   # ... as data
 *   node testing/static_surface.mjs --site excalidraw --examples 20     # ... and look at the rows
 *   node testing/static_surface.mjs --site gitlab --manifest-urls       # ... and the manifest's own set
 */

import { readFileSync, statSync, readdirSync } from "node:fs";
import { createHash } from "node:crypto";
import { relative, resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { parse } from "@babel/parser";
import { VISITOR_KEYS } from "@babel/types";
import { corpusPrograms, PROGRAM, DOCUMENT, essenceOf } from "../engine/corpus_programs.mjs";

const TAG = "static_surface";
const die = (s) => { throw new Error(`[${TAG}] ${s}`); };

/* ── THE DOORS ────────────────────────────────────────────────────────────────────────────────────────────
   Each row mirrors a call site that reaches `endpoint_record` in the engine, and names it, so that a reader
   can diff this table against `git grep -n 'endpoint_record(' engine/host` rather than trust it. `engine`
   is the file that records it; a row whose `engine` is null is a door this file reads and the engine's @H
   surface does NOT record as an endpoint, and it is counted apart for exactly that reason — mixing them
   would put addresses on the static side that the engine was never asked for, which is a comparison between
   two different questions.
   THE MATCH IS ON THE PLATFORM NAME AND NEVER ON A RECEIVER. `engine/js_code_refs.mjs` records that a
   receiver's spelling carries no information about the thing being asked, and a minifier renames every local
   while leaving `fetch`, `XMLHttpRequest`, `open` and `sendBeacon` alone because they are the platform's.
   THE GLOBAL OBJECT IS NOT A RECEIVER IN THAT SENTENCE'S SENSE, AND READING IT AS ONE COST THIS FILE
   FOURTEEN SITES. THE MATCH IS ON THE PLATFORM NAME AND NEVER ON A RECEIVER is right and it is about a
   receiver that CARRIES INFORMATION —
   `api.fetch`, `this.fetch`, `mk().fetch`, where the name to the left is a minifier's local and says
   nothing about what is being asked. A reference to the GLOBAL OBJECT is the opposite of that: `fetch(u)`
   and `window.fetch(u)` are ONE platform name reached two ways, and what stands to the left is a spelling
   of the global scope rather than an object whose identity is in question. So a member call whose object is
   PROVABLY the global object is looked up in the `callee-global` and `new` rows exactly as if the object had
   not been written, and the same holds for `new self.Worker`.
   PROVABLY IS THE LOAD-BEARING WORD AND IT IS THE SAME DISCIPLINE `collectBinds` APPLIES TO A REFERENCE.
   A reference that resolves to NO BINDING, in a file that never assigns that name as a free one either,
   cannot be anything but the global; a reference that resolves to a binding can mean something else, and the
   UMD wrapper `(function(window){ ... })(window)` and the transpiler idiom `var self = this` both do. Such a
   site is REFUSED and the refusal carries a size rather than a sentence, which is what
   `xhrOpenSkippedNonLiteralMethod` already does for the xhr.open method exclusion.
   THIS USED TO ASK WHETHER THE FILE BINDS THE NAME ANYWHERE, AND THAT IS THE READING A READER RE-DERIVES, so
   it is written down rather than deleted: a name bound ZERO times in the whole file and assigned never cannot
   be anything but the global. True, and far narrower than the question — ONE UMD factory anywhere in a
   megabyte of bundle refused every `window.X` in it. The scope pre-pass asks about the REFERENCE, which is
   what `provably` always meant, and the price is printed on every run: the refusal count is what a reader
   checks it against, and it is a count of references whose `window` really does resolve to something.
   THE THREE NUMBERS THAT PRICE THIS ARE PRINTED ON EVERY RUN AND NONE OF THEM IS ASSERTED HERE, because a
   widening that buys recall is only honest beside what it declines, and a figure over a corpus this
   repository does not carry cannot be checked by a reader who re-derives it. `globalDoor` reports what was
   ADMITTED through the global spelling, what was REFUSED for a bound global name, and how many member calls
   and constructions naming a platform door were DECLINED for a receiver that is not the global object at
   all — which is the library-wrapper population WHAT IS NOT HERE AND WHY turns away, counted instead of
   described. A widening whose precision cost is unmeasured is a trade nobody made.
   THE DIRECTION THIS WIDENS IN IS THE ONE THAT COSTS THIS PROJECT, WHICH IS THE WHOLE REASON IT BELONGS IN
   A CONTROL. A recall hole here makes the BASELINE look weak, a weak baseline makes the engine look strong,
   and this file's own opening says that is the one result it must not manufacture. So a site the parse can
   reach and this file was missing is a defect in this file however small the count, and the count moving an
   existing total is a fact to report as MOVED rather than a reason to leave the hole open.
   WHAT IS NOT HERE AND WHY, because a floor that does not say what it excludes is read as a total: a library
   wrapper (`axios.get`, `$.ajax`, an SDK's `request()`) is NOT matched. `.get(` and `.post(` are ordinary
   method names on Map, URLSearchParams, Headers and every model object in a bundle, so keying on them would
   report a number dominated by things that are not requests — the precision failure that would make this
   baseline useless in the other direction. Every request such a wrapper ultimately issues passes through
   `fetch` or `XMLHttpRequest` INSIDE the library, so the address is seen there as OPAQUE (the wrapper's own
   variable) rather than missed entirely: the effect is to move rows from SHAPE/LITERAL into OPAQUE, which
   UNDERSTATES what a parse recovers. That is the safe direction for this file and it is still a floor. */
/* EVERY DOOR CARRIES ITS DESTINATION CLASS AND THE TOTAL IS NEVER PRINTED WITHOUT IT, because a single
   count over both classes decides this question wrongly and decides it in the flattering direction.
   Fetch §2.2.5 "Requests"' DESTINATION is the concept and CLAUDE.md states it in the engine's own words —
   "a reply that becomes a PROGRAM against one that becomes a VALUE". A dynamic `import()` of a bundler chunk
   is a PROGRAM load: the page loading itself, an address a browser computes from a manifest the bundler
   emitted, and NOT an API this product exists to surface. A `fetch` is a VALUE load and is.
   MEASURED on the corpus this ran against, which is why this is a partition and not a note: 839 of 1071
   request sites were PROGRAM-door and 232 were DATA-door, so a headline over the union would be 78% a
   statement about chunk loading. Worse, the two classes have OPPOSITE kind profiles — a bundler emits its
   chunk addresses as literals or as a folded table by construction, so the PROGRAM class is where a parse
   looks strongest and it is the class the @H product cares least about. Summing them would let the easy
   population answer for the hard one. */
const DOORS = [
  { id: "fetch",         cls: "data",    engine: "browser/core/fetch/fetch.c",            kind: "callee-global",  name: "fetch",            urlArg: 0 },
  { id: "xhr.open",      cls: "data",    engine: "browser/core/xhr/xml_http_request.c",   kind: "member-call",    name: "open",             urlArg: 1, minArgs: 2, arg0Method: true },
  { id: "sendBeacon",    cls: "data",    engine: "browser/core/frame/navigator_beacon.c", kind: "member-call",    name: "sendBeacon",       urlArg: 0 },
  { id: "new WebSocket", cls: "data",    engine: null,                                    kind: "new",            name: "WebSocket",        urlArg: 0 },
  { id: "new EventSource", cls: "data",  engine: null,                                    kind: "new",            name: "EventSource",      urlArg: 0 },
  { id: "import()",      cls: "program", engine: "browser/core/html/html_script.c",       kind: "dynamic-import", name: "import",           urlArg: 0 },
  { id: "importScripts", cls: "program", engine: null,                                    kind: "callee-global",  name: "importScripts",    urlArg: 0 },
  { id: "new Worker",    cls: "program", engine: null,                                    kind: "new",            name: "Worker",           urlArg: 0 },
  { id: "new SharedWorker", cls: "program", engine: null,                                 kind: "new",            name: "SharedWorker",     urlArg: 0 },
  /* A RECEIVER-QUALIFIED DOOR, AND THE ONE CASE THE `NEVER ON A RECEIVER` RULE DOES NOT REACH. That rule is
     about a receiver whose spelling CARRIES NO INFORMATION — `api.fetch`, `this.fetch`, where the name to
     the left is a minifier's local. `serviceWorker` is the opposite: it is a PLATFORM name, so a minifier
     leaves it alone exactly as it leaves `fetch` alone, and it is the only thing that makes this door
     readable at all. `register` ALONE is not a door and cannot be one — MEASURED on the corpus this landed
     against, bare `.register(` outnumbers `navigator.serviceWorker.register(` by more than twenty to one,
     because a dependency container, an i18n catalogue and a component registry all have one. Keying on the
     property alone would report a number dominated by things that are not requests, which is the precision
     failure the DOORS comment turns `.get(`/`.post(` away for.
     IT IS A `PROGRAM` DOOR AND ITS `engine` IS NULL, so it is a FLOOR-WIDENING of the static side and not a
     comparison: Service Workers §3.4.3 "register(scriptURL, options)"' first argument is a script URL, which
     is a reply that becomes a PROGRAM, and no `endpoint_record` caller in the engine reaches it — so this row
     is counted apart exactly as `new WebSocket` and `new Worker` are. Reading it into the engine-comparable
     total would put an address on the static side the engine was never asked for.
     THIS CITED `HTML §8.10 "Service workers"` FOR ONE COMMIT AND THAT SECTION DOES NOT EXIST, which is
     recorded rather than quietly corrected because the METHOD is the finding and the number was only its
     symptom: it was written FROM MEMORY, and CLAUDE.md §A-CITED-NUMBER-CARRIES-AN-OBLIGATION rates a wrong
     number as WORSE than none, since it reads as authoritative and sends a reader to a section that does not
     say what the code claims. HTML §10 is "Web workers" and service workers are a SEPARATE STANDARD — the
     one-fetch check this file's own header demands of every citation, not made. */
  { id: "serviceWorker.register", cls: "program", engine: null,                           kind: "member-call-on", name: "register", recv: "serviceWorker", urlArg: 0 },
];
const DOOR_BY_NAME = new Map();
for (const d of DOORS) {
  const k = d.kind + ":" + d.name;
  if (DOOR_BY_NAME.has(k)) die(`two DOORS rows share ${k}`);
  DOOR_BY_NAME.set(k, d);
}
/* THE GLOBAL OBJECT'S OWN NAMES. `global` is node's and is here because a bundle ships one build for both. */
const GLOBAL_OBJECTS = new Set(["window", "self", "globalThis", "global"]);
/* WHERE THE ENGINE IS, so a name this file measures is read out of the declaration that owns it. */
const ENGINE_DIR = resolve(dirname(fileURLToPath(import.meta.url)), "..", "engine");

/* ── THE DECLARED ENTRY NAMES, AND WHY THIS FILE IS WHERE THEY GET PRICED ─────────────────────────────────
   Two components in the engine raise a census row when THE COMPILER resolves a free identifier against the
   global object, and both record the same named residual at their own declaration: the row sees ONE
   SPELLING, the bare identifier, so a program that reaches the same platform name through a property of the
   global object raises nothing. Both name the same next diff, a member-name channel at the field-get
   emitter, and both say the floor is in the direction that WITHHOLDS a finding.
   THE QUESTION THAT DIFF HAS TO BE PRICED AGAINST IS A PROPERTY OF REAL BUNDLES AND NOT OF THE ENGINE, which
   is why it is answered here and not there. Those rows are read as a BIT — zero against nonzero — and their
   own header says so in as many words, so the floor costs a READING only where a bundle spells a name
   EXCLUSIVELY as a property. A program that writes `window.requestAnimationFrame` once and the bare name
   anywhere else still raises the row, and for a bit that is the whole of what is asked of it. So the
   decisive column below is not how often the property spelling occurs; it is HOW MANY SITES SPELL A NAME
   ONLY THAT WAY, because that is the only population on which the landed denominator answers zero about a
   program that does hang work off the rung.
   THE NAMES ARE DERIVED FROM THE ENGINE AND NEVER TYPED HERE, for the reason the DOORS table gives at
   length: a list of platform names in this file would be a second copy of a fact the engine's own
   declarations already state, and the copy anyone writes first is the one that drops a name. A dropped name
   is measured as a smaller population, which is the flattering direction for the diff being priced. Each
   rung declares a NULL-terminated table at its per-realm install and each request edge declares its entry
   name as a string, so both are read out of the sources that own them and every unresolved element THROWS.
   THERE IS NO EXPECTED COUNT ASSERTED, because a count would be a bound that goes stale on the day a fourth
   rung lands: what is asserted instead is that every declaration site found resolved to at least one name,
   which grows with the tree and still fails loudly on a shape change. */
/* ONE SOURCE'S DECLARATIONS, AS A PURE FUNCTION OF ITS TEXT, SO EVERY REFUSAL BELOW CAN BE SHOWN FIRING.
   The walk that finds the files cannot be armed without a second engine tree to break; this can be armed with
   a string, and the refusals are the whole reason the population may be trusted — a shape change that went
   through quietly would report fewer declared names, and fewer names is a smaller population, which is the
   flattering direction for the diff this band exists to price. It THROWS rather than returning a short answer
   for exactly that reason. */
function entryNamesFromSource(src, where, add) {
  let rung = 0, edge = 0;
  for (const m of src.matchAll(/\brung_entry_declare\s*\(\s*([A-Z][A-Z_0-9]*)\s*,\s*([A-Za-z_][A-Za-z_0-9]*)\s*\)/g)) {
    rung++;
    const unit = m[1], table = m[2];
    const t = new RegExp(`static\\s+const\\s+char\\s*\\*\\s*const\\s+${table}\\s*\\[\\s*\\]\\s*=\\s*\\{([^}]*)\\}`).exec(src);
    if (!t) die(`${table} is declared to a rung at ${where} and this pass cannot find its table, so the ` +
                `names that rung counts would go unmeasured and the population would read smaller than it is.`);
    let got = 0;
    for (const raw of t[1].split(",").map((s) => s.trim())) {
      if (!raw || raw === "NULL") continue;
      const q = /^"(.*)"$/.exec(raw);
      if (q) { add(q[1], unit); got++; continue; }
      const lit = new RegExp(`static\\s+const\\s+char\\s+${raw}\\s*\\[\\s*\\]\\s*=\\s*"([^"]*)"`).exec(src);
      if (!lit) die(`${table}'s element ${raw} at ${where} resolves to no string literal in its own file, so ` +
                    `one declared name would be silently missing from this channel.`);
      add(lit[1], unit); got++;
    }
    if (!got) die(`${table} at ${where} resolved to no names at all.`);
  }
  for (const m of src.matchAll(/\bendpoint_([a-z_]+)_edge_declare\s*\(\s*"([^"]+)"/g)) {
    edge++; add(m[2], "edge:" + m[1]);
  }
  return { rung, edge };
}
function declaredEntryNames() {
  const host = resolve(ENGINE_DIR, "host");
  const files = [];
  const walkDir = (d) => {
    let names;
    try { names = readdirSync(d); } catch { return; }
    for (const e of names) {
      const p = resolve(d, e);
      let st; try { st = statSync(p); } catch { continue; }
      if (st.isDirectory()) walkDir(p);
      else if (/\.c$/.test(e)) files.push(p);
    }
  };
  walkDir(host);
  if (!files.length) die(`no engine source under ${host}, so the declared entry names cannot be derived and ` +
                         `a hand-typed list is the one thing this channel may not fall back to.`);
  const out = new Map();          // name -> the declaration(s) that named it
  const add = (n, who) => { if (!out.has(n)) out.set(n, []); out.get(n).push(who); };
  let rungSites = 0, edgeSites = 0, resolvedRung = 0, resolvedEdge = 0;
  for (const f of files) {
    const n = entryNamesFromSource(readFileSync(f, "utf8"), relative(ENGINE_DIR, f), add);
    rungSites += n.rung; edgeSites += n.edge; resolvedRung += n.rung; resolvedEdge += n.edge;
  }
  if (!rungSites || !edgeSites)
    die(`the declared entry names were derived from ${rungSites} rung declaration(s) and ${edgeSites} edge ` +
        `declaration(s); a zero on either side means this pass stopped matching a shape the engine still ` +
        `uses, and a channel measuring none of a population reports the smallest possible floor.`);
  if (resolvedRung !== rungSites || resolvedEdge !== edgeSites)
    die(`only ${resolvedRung}/${rungSites} rung and ${resolvedEdge}/${edgeSites} edge declaration(s) resolved.`);
  return out;
}
const ENTRY_DECL = declaredEntryNames();
const ENTRY_NAMES = new Set(ENTRY_DECL.keys());
/* THE SPELLINGS, EACH A PARTITION MEMBER EXCEPT THE LAST. `bareFree` is the one the landed rows already see;
   `bareBoundName` is a bare reference in a file that also binds the name somewhere, which the engine's
   scope-correct resolver very probably DOES see and THIS BAND'S OWN file-wide set cannot prove, so it is
   counted apart rather than folded into either answer.
   NAMED RESIDUAL — WHAT IS NOT COVERED: this band's binder set is FILE-WIDE, so `bareBoundName` holds every
   bare reference in a file that binds the name ANYWHERE, including the overwhelming majority whose binding
   is in some other function entirely. `collectBinds` now resolves references scope-correctly and could
   decide each of them, which would move most of `bareBoundName` into `bareFree`. WHAT THE NEXT DIFF BUILDS:
   this band reads `freeRef` for its bare-reference test instead of `spellBound`, keeping its own `notRef`
   population (an object key and a member name bind nothing and must stay struck) — the two sets answer
   different questions and only the binder half is replaceable. HOW ITS ABSENCE WOULD SHOW: the PROP-ONLY
   bracket printed at the foot of the per-site table stays WIDE — a `(loose)` count above its `(tight)` one —
   where a scope-correct reader would collapse the two, and the spread is the size of what is undecided.
   `typeofBare` is not a partition member and is not summed: it says
   which of the two reads the landed row would have recorded, and every one of them is already inside
   `bareFree` or `bareBoundName`. */
const SPELLINGS = ["bareFree", "bareBoundName", "qualified", "qualifiedBoundGlobal", "computedLiteral",
                   "instanceMember", "instanceMemberComputed", "destructuredFromGlobal"];
const spellTally = () => {
  const t = {};
  for (const k of SPELLINGS) t[k] = 0;
  t.typeofBare = 0;
  return t;
};
/* A PROPERTY NAME MAY NOT BE BOTH A `member-call` DOOR AND A GLOBAL-REACHED ONE, ASSERTED RATHER THAN
   BELIEVED. `window.fetch` is resolved by looking the PROPERTY up among the `callee-global` rows, so a door
   added later that names `fetch` or `importScripts` as a `member-call` would make one site match two rows
   and the order of two `if`s would decide which — the kind of silent double-count no total can reveal. The
   two families are disjoint today (`open`/`sendBeacon` against `fetch`/`importScripts`) and this is what
   keeps them so. */
for (const d of DOORS) {
  if (d.kind !== "member-call" && d.kind !== "member-call-on") continue;
  if (DOOR_BY_NAME.has("callee-global:" + d.name) || DOOR_BY_NAME.has("new:" + d.name))
    die(`DOORS names ${d.name} as a member-call AND as a global-reachable door, so a call through the ` +
        `global object would match two rows and be counted under whichever is tested first.`);
  /* AND A PROPERTY NAME MAY NOT BE BOTH AN UNQUALIFIED `member-call` AND A RECEIVER-QUALIFIED ONE, for the
     same reason and with a sharper consequence: the unqualified row would match FIRST and swallow every
     qualified site, so the receiver test that is the whole precision of the qualified row would silently
     stop being made. A row whose receiver test never runs is not a narrower door, it is `register` as a bare
     member call — the twenty-to-one population this table refuses by name. */
  if (d.kind === "member-call-on" && DOOR_BY_NAME.has("member-call:" + d.name))
    die(`DOORS names ${d.name} as both a receiver-qualified and an unqualified member-call door, so the ` +
        `unqualified row would match first and the receiver test would never run.`);
  if (d.kind === "member-call-on" && !d.recv) die(`the member-call-on door ${d.id} names no receiver`);
}

/* ── FOLDING ──────────────────────────────────────────────────────────────────────────────────────────────
   Returns { text, holes } where `text` is the literal bytes recovered with each unresolved subexpression
   rendered as `{n}` — the SAME rendering `solver/endpoint.c` uses for a concolic hole, so a static row and
   an engine row are comparable strings rather than two notations for one address — and `holes` counts them.
   `branches` collects the alternative arm of every conditional that folds both ways, which is a place a
   PARSE beats a RUN: execution takes one arm unless it forks, and the text carries both. */
const MAX_DEPTH = 24;

/* ── THE ENUMERATING FOLD: ONE FOLDER, A SECOND MODE, AND NO SECOND COPY ──────────────────────────────────
   `fold` takes an optional `env`. With `env` NULL it is the folder every existing row is classified by and
   not one of its answers moves — that is asserted rather than hoped for, by an A/B whose door totals and
   whose judged population are byte-identical across this diff. With an `env` it may additionally resolve a
   PARAMETER to a candidate value, decide an equality, index an object literal with a literal key, take the
   right arm of `||` past a missing key, and INLINE a single-parameter function. Two folders would have been
   free to disagree about what a `+` does; one folder with a mode cannot.
   WHY A MODE AND NOT A WIDER DEFAULT: an equality this file can decide is a fold the ordinary channel could
   legitimately have too, and enabling it there would MOVE numbers other lanes are pricing against. The mode
   keeps the chunk-manifest channel additive to every total that already exists, which is what makes its own
   number readable on the run it lands in.
   A MISS IS NOT AN EMPTY STRING AND THE DIFFERENCE IS THE WHOLE SOUNDNESS OF THE ENUMERATION. Indexing an
   object literal with a key it does not carry yields `undefined` in the language, and a bundler's chunk-URL
   function relies on exactly that — `({names}[id] || id)` falls through to the id. Folding a miss to `""`
   would silently INVENT an address with a segment deleted from it, so a miss carries a HOLE as well as its
   marker: read past `||` it disappears, and read anywhere else it drops the candidate. */
const MISS = () => ({ text: "{?}", holes: 1, miss: true, why: ["a map key the object does not carry"] });

/* ── A URL OBJECT IS MUTABLE, AND READING ONE OUT OF A NAME IS WHERE THAT STOPS BEING SAFE ────────────────
   THIS EXISTS BECAUSE ITS ABSENCE PRODUCED A FALSE `folded` ROW AND THAT IS THE ONE FAILURE THIS FILE MAY
   NOT HAVE. `new URL(x)` folds to an exact string, and a `URL` is not a string: `r.searchParams.set(...)`
   MUTATES it in place, so a later `r.toString()` is the constructed address PLUS a query the parse never
   read. Reporting the constructed address as COMPLETE is worse than reporting nothing — a control that
   claims to resolve an address the program does not send makes the engine look worse than it is, which is
   the mirror of the result this file's opening forbids and is equally dishonest.
   MEASURED, at the corpus this landed against, and the shape rather than the site because a site rots: one
   bundle wrote `let r = new URL(<literal>); r.searchParams.set(...) ×5; fetch(r.toString())`, and the row
   read `folded` with a complete `https://…/watermark-track.svg` for a request that carries five query
   parameters. It was found by reading the source at every newly-settled row rather than by any test here.
   WHERE THE HOLE IS ADDED IS THE WHOLE PRECISION OF IT, and it is not "always". A construction that is the
   DIRECT operand of its consumer — `fetch(new URL("/a", base))` — has never been bound to anything, so no
   statement can have reached it and its fold is exact. Mutation needs a NAME to mutate through, so the hole
   is added exactly where a name, a slot or an object-literal property is DEREFERENCED, which is the set of
   places a value acquires one. That keeps the inline construction `folded` and makes the bound one a SHAPE,
   which is the true reading of each.
   IT IS A FLOOR AND NOT A CLAIM THAT MUTATION HAPPENED. A bound `URL` nothing mutates is reported as a
   SHAPE where the text is in fact complete, which understates what the parse recovered — the direction
   CLAUDE.md §A-SWEEP-IS-TRUSTED-BY-ITS-METHOD requires of this file, since the lower bound belongs to the
   baseline. Proving the absence of mutation is a second slot analysis and is the named residual below. */
/* THE FLOOR'S OWN PRICE IS COUNTED, WHICH IS WHAT MAKES THE RESIDUAL ABOVE A MEASUREMENT RATHER THAN A
   CLAIM. Every demotion is a row whose text the fold recovered completely and which this file reports as a
   SHAPE, so the count is the exact CEILING on what an escape analysis over the URL's binding could ever
   recover — and the rows are printed with their `file:line` so the question "is this URL really mutated"
   is answered by reading the source rather than by believing a sentence here. */
const deref = (r, env) => {
  if (!r.mut) return r;
  if (env && env.mutFloor) env.mutFloor.demoted++;
  /* THE DEMOTION'S OWN HOLE IS A MUTATION NOTHING IN THE TEXT RECORDS, which is a mechanism of its own and
     not one of the classes above — a URL object's setters and its searchParams leave no byte behind. */
  return { text: r.text + "{?}", holes: r.holes + 1, why: WHY(r).concat(["a mutation nothing in the text records"]) };
};

/* ── WHAT AN ADDRESS LOOKS LIKE, IN ONE PLACE ─────────────────────────────────────────────────────────────
   The base rate and the chunk manifest both have to decide whether a recovered string is an address, and two
   copies of that decision would be free to drift into reporting one population under two definitions. The
   first two alternatives are the base rate's own, unchanged, so its number cannot move by this being
   factored out; the third is added for the manifest, because a bundler that emits `./chunk.HASH.js` has
   written an address and the base rate never had to read one.
   A FRAGMENT IS REFUSED AND THAT IS THE POINT RATHER THAN A LIMITATION. A composition that recovers a chunk
   NAME without the public path in front of it — `grafana.geomapPanel.HASH.css`, `chunk.123.js` — has
   recovered part of an address, and emitting a part as a whole would be this channel INVENTING one. Such a
   fold is counted as a FRAGMENT and reported apart: the count says a manifest is present and that the
   composition this file reached did not include its public path, which is a floor stated with a size.
   IT IS ALSO THE PRECISION HALF OF THE MANIFEST CHANNEL AND IT WAS MEASURED, NOT ASSUMED. Without it the
   channel reported `session`, `Users`, `usdc-usdt-n` and `0.001` as addresses — a one-parameter function
   indexing a string table is an i18n table, an enum or a label map at least as often as it is a chunk
   manifest, and nothing about the SHAPE of the code tells them apart. What tells them apart is what comes
   out, which is the only axis that does not require knowing whose runtime wrote it. Ground truth for the
   direction: of the addresses recovered at one site, fifteen name files the fetcher independently mirrored. */
const looksLikeAddress = (v) => /^https?:\/\/[^\s]+$/.test(v) ||
                                /^\/[A-Za-z0-9_][^\s"'<>]*$/.test(v) ||
                                /^\.{1,2}\/[^\s"'<>]+$/.test(v);

/* ── WHY AN ADDRESS IS INCOMPLETE — THE ONE CLASSIFICATION PLACE ─────────────────────────────────────────
   WHAT THIS ANSWERS AND WHY IT IS A PARTITION RATHER THAN A WIDENING. `opaque` is correct and
   uninformative: it cannot tell an address composed from a value ONLY A RUN HAS from an address composed
   from a value a run has AND THE ENGINE ALREADY LEARNS. A reply body's fields are the second, and the
   comparison this file is one half of wants the razor quoted as `what the engine reaches that no parse can,
   with the mechanism named per row` rather than asserted in aggregate. So every hole carries the CLASS of
   the thing that would have filled it, and the rows are bucketed by the SET of classes they carry.
   IT MOVES NO ROW AND THAT IS BY CONSTRUCTION, SO IT MAY NOT BE PRICED BY WHAT IT RESOLVES. A partition
   that moved a total would have become a second count of the same population, which is the shape two
   classifications of one thing always drift into — the check is that every figure above is byte-identical
   across the diff that adds this, and the band's own buckets are ASSERTED to sum to `shape + opaque`.
   THE CLASS IS ATTACHED AT THE ORIGIN AND NEVER INFERRED AT THE ROW, which is the whole of its soundness.
   A door written inside a `.then` callback whose address is `"/api/" + someGlobal` is NOT reply-derived, so
   a band keyed on what LEXICALLY ENCLOSES the call would have said it was. The hole knows where the fold
   gave up; the row does not.
   WHAT IT DELIBERATELY DOES NOT CLAIM. `.then` is classified as A PROMISE RESOLUTION VALUE and never as a
   REPLY, because the resolution value of `p.then(f)` is whatever `p` settles with and that is a fact about
   `p` rather than about `then`. Upgrading it would need the receiver traced to a data-door call, which is a
   real analysis and is the named residual below rather than a word chosen here. */
const VALUE_SUPPLIERS = new Map([
  /* A PLATFORM CALLEE WHOSE OWN SEMANTICS BIND ITS CALLBACK'S PARAMETER. The table names WHAT THE PARAMETER
     IS, which is what the classification turns on, and cites no section because the class does not depend on
     one — a claim that needed a citation would be a claim about behaviour this file implements, and this file
     implements none of these.
     `setTimeout` AND `setInterval` ARE DELIBERATELY ABSENT AND THAT IS PRECISION RATHER THAN AN OMISSION:
     their callback receives the EXTRA ARGUMENTS THE CALLER PASSED, so the value is app-supplied or absent
     and the platform states nothing about it. Listing them would have attributed a caller's own value to a
     platform boundary, which is the direction that overstates what only a run can have. */
  ["then",        "a promise resolution value"],
  ["catch",       "a promise resolution value"],
  ["finally",     "a promise resolution value"],
  ["map",         "an array element"],
  ["forEach",     "an array element"],
  ["filter",      "an array element"],
  ["find",        "an array element"],
  ["findIndex",   "an array element"],
  ["some",        "an array element"],
  ["every",       "an array element"],
  ["flatMap",     "an array element"],
  ["reduce",      "an array element"],
  ["sort",        "an array element"],
  ["addEventListener", "an Event"],
  ["requestAnimationFrame", "a platform timestamp"],
  ["requestIdleCallback",   "a platform timestamp"],
]);
const HOLE_CLASSES = [
  "a promise resolution value", "an array element", "an Event", "a platform timestamp",
  "a callback parameter supplied by app code", "a callback parameter whose supplier this pass cannot name",
  "a function parameter, no platform supplier",
  "a global this file never assigns", "a name this pass refuses to settle",
  "a property read", "a computed property read", "a map key the object does not carry",
  "a call result", "an operator this fold does not evaluate", "a node kind this fold has no arm for",
  "a mutation nothing in the text records",
];
/* THE CLASSIFIER. One function, called from every hole-producing return, so the classes cannot be stated in
   two places and disagree. A class it cannot name is the LAST member of the list and is printed as such on
   the clean day — an unattributed bucket is better than a forced classification, because a forced one is a
   claim about a mechanism nobody established. */
function holeClass(node, env) {
  if (!node) return "a node kind this fold has no arm for";
  switch (node.type) {
    case "Identifier": {
      const ps = env && env.paramOf ? env.paramOf.get(node) : null;
      if (ps) {
        if (env.cbVia && env.cbVia.has(ps.fn)) {
          const via = env.cbVia.get(ps.fn);
          if (!via) return "a callback parameter whose supplier this pass cannot name";
          return VALUE_SUPPLIERS.get(via) || "a callback parameter supplied by app code";
        }
        /* EVERY OTHER PARAMETER IS CALLER-SUPPLIED AND IS ONE CLASS HERE ON PURPOSE. Which KIND of caller —
           a shared helper, a method reached through a receiver, a function assigned to a property — is the
           partition the closure's own refusal block already prints, and a second copy of it here would be
           the two-classifications-of-one-thing shape that drifts. */
        return "a function parameter, no platform supplier";
      }
      if (env && env.freeRef && env.freeRef.has(node)) return "a global this file never assigns";
      return "a name this pass refuses to settle";
    }
    case "MemberExpression": case "OptionalMemberExpression":
      return node.computed ? "a computed property read" : "a property read";
    case "CallExpression": case "OptionalCallExpression": case "NewExpression":
      /* THE SECOND FIELD IS SEEDED HERE AND THE CLASS DOES NOT MOVE. A Set keyed on the NODE is what makes the
         census a count of call SITES rather than of folds: the manifest enumeration folds one call once per
         candidate, so a tally would have counted the same site dozens of times and read as a population. */
      if (env && env.callHoles) env.callHoles.add(node);
      return "a call result";
    case "BinaryExpression": case "LogicalExpression": case "UnaryExpression":
      return "an operator this fold does not evaluate";
    default:
      return "a node kind this fold has no arm for";
  }
}
const H = (node, env) => ({ text: "{?}", holes: 1, why: [holeClass(node, env)] });
const WHY = (r) => r.why || [];
/* THE ROW'S SIGNATURE IS THE SORTED SET OF ITS HOLES' CLASSES, NOT A RANKED PICK. A ranking would have let
   a row with one promise hole and four global holes read as a promise row, which is a convention nobody can
   check; a set is a fact about the row. It also keeps the buckets summing exactly to `shape + opaque`,
   because every hole has a class and every incomplete row has a hole — which is the one property that says
   this is a partition and not a second count. */
const holeSig = (r) => {
  const w = [...new Set(WHY(r))].sort();
  return w.length ? w.join(" + ") : "a class this fold did not attach";
};

/* ── THE SECOND FIELD ON `a call result`: IS THE CALLEE'S OWN DEFINITION IN THIS FILE'S TEXT ──────────────
   WHAT THIS ANSWERS AND WHY THE BAND CANNOT. The WHY band says WHERE THE FOLD GAVE UP, and its largest class is
   `a call result`. THAT CLASS IS NOT "EVERY CALL" AND THIS FILE'S FIRST DRAFT OF THIS COMMENT SAID IT WAS: the
   door channel's inline arm DOES cross the callee boundary, and where it succeeds the hole is attributed INSIDE
   the callee at its own origin, so such a row never carries `a call result` at all. The class is therefore
   EXACTLY THE POPULATION THAT ARM REFUSED — a body of more than one statement, a mismatched arity, an argument
   that did not settle, a member callee whose slot no scope-correct resolution can join — and this census is a
   second field on THAT, not on calls in general. The distinction is not pedantry: the first framing would have
   had a reader believe the baseline crosses no function boundary, which UNDERSTATES it, and understating the
   baseline is the direction that flatters the engine. A reader cannot tell from a refusal reason whether the
   address is in the bundle's text anyway, and that is the only question the razor turns on. So this asks a PROPERTY each row can be tested for
   by hand: does the callee resolve to ONE definition in this file, and does that definition's return fold to
   text from this file alone. A reader checks a row by opening the coordinate printed beside it.
   IT IS A SECOND FIELD AND NEVER A RELOCATION. `a call result` stays in the band and the band's buckets go on
   summing to `shape + opaque` for their channel; this census is printed beside them with its OWN unit. Turning
   the class into sub-classes instead would have moved the number and left its old meaning unread, which is the
   defect CLAUDE.md names for a relocated metric.
   ITS UNIT IS A CALL EXPRESSION AND NOT A ROW OR AN ADDRESS, and the three are not subtractable. One door row
   can carry several call holes and one call node can be a hole in several rows (a `.src` write and a `fetch`
   reading the same helper), so this census's total is neither the band's `a call result` figure nor any door
   count. It is deduplicated BY NODE IDENTITY within a file, which is what makes it immune to the manifest
   enumeration folding one call once per candidate.
   WHAT IT DELIBERATELY DOES NOT CLAIM, AND WHICH WAY IT ERRS. `its return folds to text from this file alone`
   is a property OF THE DEFINITION and never a verdict about what an extractor would recover: a real
   interprocedural fold would additionally have to establish that no other definition can be entered through
   that callee, and the once-declared/never-reassigned conditions `binds`, `fnDeclOf` and `slotOf` already
   carry are what stands in for that here. Where this is wrong it is wrong by ADMITTING TOO MUCH — a call
   whose fold was discarded speculatively is still counted, and a definition whose return folds is credited
   even where a run would return by another path — and that direction OVERSTATES what a parse could reach,
   which is the conservative direction for this file's own bar: it is UNDERSTATING the baseline that would
   flatter the engine, and CLAUDE.md names that as the one result this project must not manufacture. */
const CALLEE_IN_FILE = "its return folds to text from this file alone";
/* THE TWO VERDICTS THAT MEAN THE CALLEE WAS RESOLVED TO ONE DEFINITION IN THIS FILE, which is the population
   whose coordinates are printed. A ZERO IN THE FIRST IS THE HEADLINE AND IS THEREFORE THE CLAIM MOST IN NEED
   OF CHECKING, and a zero with nothing beside it gives a reader nothing to falsify — so the rows of the SECOND
   are printed too. They are the stronger statement of the pair: a definition IS in this file's text and its
   return is NOT text, which anybody can refute by opening one coordinate. */
const CALLEE_NOT_TEXT = "its return does not fold from this file's text";
const CALLEE_VERDICTS = [
  CALLEE_IN_FILE,
  "its return does not fold from this file's text",
  "its definition returns nothing this pass can read",
  "the callee resolves to a value that is not a function",
  "the callee is a name this file never binds",
  "the callee is a name this pass will not settle",
  "the callee is a property this pass cannot settle",
  "the callee is a computed property read",
  "the callee is not a name at all",
  "it constructs an object rather than returning a value",
];
const spellCallee = (c) => {
  if (!c) return "?";
  if (c.type === "Identifier") return c.name;
  if ((c.type === "MemberExpression" || c.type === "OptionalMemberExpression") && !c.computed)
    return (c.object.type === "Identifier" ? c.object.name : "?") + "." +
           (c.property.type === "Identifier" ? c.property.name : "?");
  return "<" + c.type + ">";
};
/* THE RETURNS OF THIS FUNCTION AND NOT OF THE ONES INSIDE IT. A nested function's `return` says nothing about
   what the outer call evaluates to, so the walk carries its own function-nesting depth rather than collecting
   every `ReturnStatement` in the subtree — which would have credited a helper with a URL that belongs to a
   closure it merely returns. A concise arrow body IS the return and has no statement to find. */
function ownReturns(fn) {
  if (!fn || !fn.body) return null;
  if (fn.body.type !== "BlockStatement") return [{ argument: fn.body }];
  const out = []; let d = 0;
  walk(fn.body, (n) => {
    if (FN_LIKE.has(n.type)) d++;
    else if (d === 0 && n.type === "ReturnStatement") out.push(n);
  }, (n) => { if (FN_LIKE.has(n.type)) d--; });
  return out;
}
/* WHETHER A RESOLVED CALLEE'S RETURN DEPENDS ON WHAT THE CALLER PASSED — the one bit that sorts the rows this
   census resolves into TWO DIFFERENT next diffs, and the reason a reader must not read them as one lump.
   `return o.p + MAP[e] + ".chunk.js"` holes on the PARAMETER `e`, and that shape is the bundler chunk
   function this file's MANIFEST channel already enumerates — so the address IS in the text, per candidate,
   under another unit. `return x` where `x` is a local the body assembled holes on nothing the caller can
   supply, and no amount of call-site information settles it; what would is a fold over the callee's own
   straight-line writes.
   IT IS RESOLVED THROUGH `paramOf` AND NEVER BY NAME, so a name that SHADOWS a parameter inside the body is
   not counted as one. That also makes it UNDER-count: `paramOf` excludes a parameter the body rewrites and
   every parameter of a function that reads `arguments`, so a return depending on one of those reads as
   caller-independent. The direction is deliberate — under-counting parameter dependence OVER-states how much
   recall is left for a stronger parse to take, and it is OVER-stating the baseline that is the safe error
   here, because understating it is what would flatter the engine.
   IT IS A FACT ABOUT THE RETURN EXPRESSION AND NOT ABOUT TRANSITIVE DEPENDENCE, WHICH THIS COMMENT FIRST GOT
   WRONG BY CALLING THE OTHER SIDE "SETTLED BY NO CALL-SITE INFORMATION". Reading three of the rows it puts on
   that side refuted it: each returns a LOCAL that is an alias of a parameter one assignment earlier — `let
   e=r; ... return e`, `let t; try{t=new URL(e)}... return t`, `var i=e.split("?")[0]; return "".concat(i,…)`
   — so the caller's value IS reachable, one straight-line write away. The claim would have read as "nothing can
   be done here", which is the direction that flatters the engine, and it was wrong on 3 of 3 rows opened.
   WHAT THE BIT THEREFORE MEANS is narrower and still worth having: whether the caller's value is reachable
   WITHOUT a fold over the callee's own writes. Both sides reduce to ONE next diff — that fold — and the bit
   says which of them it is needed FOR. */
function returnUsesOwnParam(fn, rets, env) {
  if (!env || !env.paramOf) return false;
  let hit = false;
  for (const r of rets) {
    if (!r.argument) continue;
    walk(r.argument, (n) => {
      if (hit || n.type !== "Identifier") return;
      const ps = env.paramOf.get(n);
      if (ps && ps.fn === fn) hit = true;
    });
  }
  return hit;
}
/* EVERY RETURN MUST FOLD, NOT THE FIRST ONE. A helper with `return "/a"` on one path and `return u` on another
   evaluates to text only on one of them, so crediting it from its first return would have claimed an address
   the call need not produce. A BARE `return;` is the same refusal spelled shorter: the call can evaluate to
   `undefined`, which is not text. */
function calleeVerdict(call, binds, env) {
  const no = (v) => ({ verdict: v, text: null });
  if (call.type === "NewExpression") return no("it constructs an object rather than returning a value");
  const c = call.callee;
  if (!c) return no("the callee is not a name at all");
  let fn = null;
  if (c.type === "Identifier") {
    const b = binds.get(c), fd = env.fnDeclOf.get(c);
    if (fd) fn = fd;
    else if (b && b.node && FN_LIKE.has(b.node.type)) fn = b.node;
    else if (b) return no("the callee resolves to a value that is not a function");
    else if (env.freeRef.has(c)) return no("the callee is a name this file never binds");
    else return no("the callee is a name this pass will not settle");
  } else if (c.type === "MemberExpression" || c.type === "OptionalMemberExpression") {
    if (c.computed) return no("the callee is a computed property read");
    const v = env.slotOf.get(c);
    if (v && FN_LIKE.has(v.type)) fn = v;
    else if (v) return no("the callee resolves to a value that is not a function");
    else return no("the callee is a property this pass cannot settle");
  } else return no("the callee is not a name at all");
  const rets = ownReturns(fn);
  if (!rets || !rets.length) return no("its definition returns nothing this pass can read");
  const dep = () => ({ verdict: CALLEE_NOT_TEXT, text: null, param: returnUsesOwnParam(fn, rets, env) });
  let first = null;
  for (const r of rets) {
    if (!r.argument) return dep();
    const got = fold(r.argument, binds, 0, env);
    if (got.holes !== 0) return dep();
    if (first === null) first = got.text;
  }
  return { verdict: CALLEE_IN_FILE, text: first, param: false };
}

function fold(node, binds, depth, env) {
  if (node == null) return H(null, env);
  if (depth > MAX_DEPTH) return { text: "{?}", holes: 1, why: ["a node kind this fold has no arm for"] };
  if (env) {
    const r = foldEnvOnly(node, binds, depth, env);
    if (r) return r;
  }
  switch (node.type) {
    case "StringLiteral":
      return { text: node.value, holes: 0 };
    case "NumericLiteral":
      return { text: String(node.value), holes: 0 };
    case "TemplateLiteral": {
      let t = "", h = 0, w = [];
      for (let i = 0; i < node.quasis.length; i++) {
        t += node.quasis[i].value.cooked ?? node.quasis[i].value.raw ?? "";
        if (i < node.expressions.length) {
          const r = fold(node.expressions[i], binds, depth + 1, env);
          t += r.holes ? r.text : r.text;
          h += r.holes;
          w = w.concat(WHY(r));
        }
      }
      return { text: t, holes: h, why: w };
    }
    case "BinaryExpression": {
      if (node.operator !== "+") return H(node, env);
      const a = fold(node.left, binds, depth + 1, env), b = fold(node.right, binds, depth + 1, env);
      return { text: a.text + b.text, holes: a.holes + b.holes, why: WHY(a).concat(WHY(b)) };
    }
    case "Identifier": {
      /* THE BINDING THIS REFERENCE MEANS, by node identity rather than by name — see `collectBinds`. */
      const b = binds.get(node);
      if (b && b.node) return deref(fold(b.node, binds, depth + 1, env), env);
      return H(node, env);
    }
    case "MemberExpression": {
      /* An object whose binding resolves to an object literal with literal keys — `const R={u:"/x"};
         fetch(R.u)`. */
      if (node.computed || node.object.type !== "Identifier" || node.property.type !== "Identifier")
        return H(node, env);
      const b = binds.get(node.object);
      if (!b || !b.node || b.node.type !== "ObjectExpression") return H(node, env);
      for (const p of b.node.properties) {
        if (p.type !== "ObjectProperty" || p.computed) continue;
        const k = p.key.type === "Identifier" ? p.key.name : (p.key.type === "StringLiteral" ? p.key.value : null);
        if (k === node.property.name) return deref(fold(p.value, binds, depth + 1, env), env);
      }
      return H(node, env);
    }
    case "ConditionalExpression": {
      const a = fold(node.consequent, binds, depth + 1, env), b = fold(node.alternate, binds, depth + 1, env);
      if (a.holes === 0 && b.holes === 0) return { text: a.text, holes: 0, alt: b.text };
      /* BOTH ARMS' CLASSES, because a ternary whose arms fail for two reasons carries two mechanisms and a
         band that kept one would name whichever arm the fold happened to read first. */
      return { text: "{?}", holes: 1, why: WHY(a).concat(WHY(b)) };
    }
    case "TSAsExpression":
    case "TSNonNullExpression":
    case "ParenthesizedExpression":
      return fold(node.expression, binds, depth + 1, env);
    case "NewExpression":
    case "CallExpression":
    case "OptionalCallExpression": {
      const r = foldPlatformString(node, binds, depth, env);
      return r || H(node, env);
    }
    default:
      return H(node, env);
  }
}

/* ── THE PLATFORM'S OWN STRING-VALUED CONSTRUCTIONS ───────────────────────────────────────────────────────
   WHY THIS EXISTS: `new URL(...)` WAS A LEAF, AND A LEAF OCCLUDES EVERYTHING BEHIND IT. The fold stopped at
   any construction, so `new Worker(new URL(n.p + n.u(1298), n.b))` — webpack 5's worker spelling — was read
   as OPAQUE with the chunk-id composition INSIDE it never folded at all. That is the shape CLAUDE.md
   §AND-A-FINDING-CAN-OCCLUDE-ITS-OWN-SUCCESSOR names: the outer node is one row of a finding AND it removes
   the only route to the composition underneath, so the address is not merely uncounted, it is unreachable BY
   the row that should have led to it. MEASURED at the corpus this landed against and stated as a shape
   rather than a frozen number, since a count over a corpus this repository does not carry cannot be
   re-derived: `new URL` was the single largest argument construction at the PROGRAM door and every one of
   them read OPAQUE, and `.toString()` was the largest at the DATA door.
   THE NAME MUST BE THE PLATFORM'S, WHICH IS THE SAME TEST THE GLOBAL DOOR MAKES AND NOT A SECOND ONE.
   `URL`, `Request` and `String` are read through `freeRef` — a reference that resolves to no binding this
   file makes and whose name nothing assigns free — so a bundle that ships its own `URL` polyfill and binds
   the name is REFUSED rather than folded through a constructor whose behaviour is not the standard's.
   WHAT IS REFUSED AND WHY, because a coercion set that does not say what it turns away is read as complete:
   `encodeURIComponent`, `encodeURI` and `.replace()` CHANGE the bytes, so folding them to their operand
   would report an address the program never sends; `.concat()` cannot be told from `Array.prototype.concat`
   by any parse, so admitting it would fold an array join into a URL; `.toString(radix)` is not identity and
   is refused by its argument count. Each of those is a fold a stronger tool could make with a type it does
   not have, and each is left as a hole. */
const ABSOLUTE_URL = /^[A-Za-z][A-Za-z0-9+.\-]*:/;
const URL_UNKNOWN_BASE = "https://static-surface.invalid/";
function foldPlatformString(node, binds, depth, env) {
  if (!env || !env.freeRef) return null;
  const c = node.callee;
  const isNew = node.type === "NewExpression";
  const args = node.arguments || [];
  const globalName = c && c.type === "Identifier" && env.freeRef.has(c) ? c.name : null;

  /* `new URL(ref)` / `new URL(ref, base)` — THE URL STANDARD'S OWN RESOLUTION, APPLIED BY THE PARSE. This is
     not a guess about a runtime value: resolving one string against another is a PURE FUNCTION of the two,
     the same function a browser applies, and node's `URL` is that function. Where both operands are recovered
     literally the answer is EXACT and the row is `folded`.
     WHERE THE BASE IS UNKNOWN THE ANSWER IS A SUFFIX AND THE ROW IS A SHAPE, which is the strongest honest
     thing a parse can say and is true rather than approximately true: resolution APPENDS a relative
     reference's non-`..` portion verbatim to whatever the base's directory turns out to be, and removes
     segments only from the BASE — so the resolved URL really does end in the bytes printed after the hole,
     for a root-relative, directory-relative and `..`-climbing reference alike. A reference beginning `?` or
     `#` is the one shape that is NOT a suffix — it keeps the base's own path — and is refused. */
  if (isNew && globalName === "URL") {
    if (!args.length || args.length > 2) return null;
    const ref = fold(args[0], binds, depth + 1, env);
    if (ref.holes !== 0) return null;
    if (ABSOLUTE_URL.test(ref.text)) {
      try { return { text: new URL(ref.text).href, holes: 0, mut: true }; } catch { return null; }
    }
    if (args.length === 2) {
      const base = fold(args[1], binds, depth + 1, env);
      if (base.holes === 0 && ABSOLUTE_URL.test(base.text)) {
        try { return { text: new URL(ref.text, base.text).href, holes: 0, mut: true }; } catch { return null; }
      }
    }
    if (ref.text.startsWith("?") || ref.text.startsWith("#") || ref.text === "") return null;
    try {
      const u = new URL(ref.text, URL_UNKNOWN_BASE);
      if (u.origin !== new URL(URL_UNKNOWN_BASE).origin) return null;
      return { text: "{?}" + u.pathname + u.search + u.hash, holes: 1, mut: true,
               why: [holeClass(args[1] || null, env)] };
    } catch { return null; }
  }
  /* `new Request(input)` — Fetch §2.2.5's own constructor, whose first argument is the address and which
     resolves a relative one exactly as `fetch` does, so folding to the operand's text is the SAME convention
     every `fetch("/api")` row in this file already uses. A `Request` handed another `Request` folds to
     whatever that one folds to, which is the same answer one level in. */
  if (isNew && globalName === "Request") {
    if (args.length < 1) return null;
    return fold(args[0], binds, depth + 1, env);
  }
  if (isNew) return null;
  /* `String(x)` — identity on a string and the standard's coercion on anything else, and `x` only folds at
     all when it is a literal or a composition of literals, so the recovered text IS what the call returns. */
  if (globalName === "String" && args.length === 1) return fold(args[0], binds, depth + 1, env);
  /* `x.toString()` WITH NO ARGUMENT — identity on a string, and the reason this arm matters is that it is how
     a bundle spells the end of a `URL` builder: `fetch(u.toString())`. With an argument it is a radix and is
     not identity, which the count test refuses. The receiver is not asked to be the platform's anything: it
     has to FOLD, and the only things that fold are literals and compositions of them. */
  if (args.length === 0 && c && (c.type === "MemberExpression" || c.type === "OptionalMemberExpression") &&
      !c.computed && c.property.type === "Identifier" && c.property.name === "toString")
    return fold(c.object, binds, depth + 1, env);
  return null;
}

/* `obj.prop` AS ONE KEY, so a write and a read of the same slot are the same string and cannot drift. */
function memberKey(node) {
  if (!node || (node.type !== "MemberExpression" && node.type !== "OptionalMemberExpression")) return null;
  if (node.computed || node.object.type !== "Identifier" || node.property.type !== "Identifier") return null;
  return node.object.name + "." + node.property.name;
}

/* AN OBJECT LITERAL, HOWEVER THE FILE SPELLS THE WAY TO IT: written inline, which is what a minified chunk
   table is; bound to a name whose binding resolves here; or assigned once to a property. */
function objectLiteralOf(node, binds, env) {
  if (!node) return null;
  if (node.type === "ObjectExpression") return node;
  if (node.type === "Identifier") {
    const b = binds.get(node);
    return b && b.node && b.node.type === "ObjectExpression" ? b.node : null;
  }
  if (env) {
    const m = env.slotOf.get(node);
    if (m && m.type === "ObjectExpression") return m;
  }
  return null;
}

/* A FUNCTION WHOSE WHOLE BODY IS ONE RETURNED EXPRESSION, which is the only shape that can be inlined
   without reasoning about statements. A function with more than one statement is REFUSED rather than
   approximated by its last return: the statements before it may narrow the parameter, and a fold that
   ignored them would enumerate addresses the function cannot actually return.
   IT IS ONE HELPER FOR TWO CHANNELS AND THE ARITY TEST IS THE CALLER'S, which is what keeps the manifest's
   enumeration and the door channel's inline from drifting into two definitions of "inlinable": the manifest
   asks for ONE parameter because it binds a candidate to it, and the door channel asks for the arity its own
   call site supplies. A second copy of this resolution would have been free to disagree about which of the
   three ways a file spells the way to a function it trusts.
   AN `async` OR GENERATOR FUNCTION IS REFUSED, AND THAT REFUSAL IS THIS HELPER'S AND NOT A CHANNEL'S. Its
   call does not evaluate to the body's value — it evaluates to a Promise or an iterator — so inlining one
   substitutes the string the body would eventually produce for an object whose text is `[object Promise]`.
   That is not a shorter answer but a FABRICATED address, and it is the one failure mode an inline has that
   cannot be read off the recovered text: the string looks exactly like an address because it IS the address
   the program would have used one `await` later. Its price is printed on every run beside what the inline
   buys, which is what makes this a trade rather than an assertion. */
function returnExprFnOf(node, binds, env) {
  let fn = null;
  if (!node) return null;
  /* `(0, f)(x)` IS `f(x)`, AND THIS IS NOT A WIDENING OF WHAT MAY BE ASSUMED. A sequence expression
     evaluates to its LAST operand, which the language says and which is the whole of the claim; every
     transpiler in this corpus emits the form deliberately, to strip a member call's `this` so that
     `(0, ns.f)(x)` calls `f` with `this` undefined. Refusing it does not withhold an assumption, it refuses
     to read a spelling — and the shape is common enough that its absence was the third-largest reason this
     inline declined. */
  if (node.type === "SequenceExpression" && node.expressions.length)
    return returnExprFnOf(node.expressions[node.expressions.length - 1], binds, env);
  /* A FUNCTION WRITTEN AT THE CALL — `(x => "/a/" + x)(1)`. There is no binding to resolve and therefore
     nothing to be wrong about: the callee IS the function, so the resolution that the three spellings below
     exist to perform has already happened. Its absence was a refusal to read the one callee shape that
     needs no resolution at all. */
  if (node.type === "FunctionExpression" || node.type === "ArrowFunctionExpression") fn = node;
  else if (node.type === "Identifier") {
    const b = binds.get(node);
    if (b && b.node) fn = b.node;
    /* A FUNCTION DECLARATION IS ONE FUNCTION ONLY IF THIS REFERENCE RESOLVES TO IT, and `binds` cannot
       answer for it: a declaration has no initializer, so `collectBinds` files it under the same "not
       foldable" set as a parameter. `fnDeclOf` is the same resolution asked separately, and it is asked of
       the REFERENCE — minified code reuses one letter for a dozen declarations in a dozen scopes, and
       inlining whichever of them a name-keyed map happened to hold would enumerate addresses no call site
       can produce. */
    else if (env && env.fnDeclOf.has(node)) fn = env.fnDeclOf.get(node);
  } else if (env) {
    const m = env.slotOf.get(node);
    if (m) fn = m;
  }
  if (!fn) return null;
  if (fn.type !== "FunctionDeclaration" && fn.type !== "FunctionExpression" &&
      fn.type !== "ArrowFunctionExpression") return null;
  if (fn.async || fn.generator) return { fn, refused: "async" };
  if (!fn.params) return null;
  if (fn.body.type === "BlockStatement") {
    if (fn.body.body.length !== 1) return null;
    const st = fn.body.body[0];
    if (st.type !== "ReturnStatement" || !st.argument) return null;
  }
  return { fn, refused: null };
}
const returnExprOf = (fn) => fn.body.type === "BlockStatement" ? fn.body.body[0].argument : fn.body;
/* A NESTED FUNCTION INSIDE THE EXPRESSION BEING INLINED IS REFUSED, and the reason is the one thing a
   name-keyed substitution can get wrong. `env.vars` maps a parameter NAME to a value, and a function written
   inside the returned expression may BIND THAT NAME AGAIN — `x => (x => "/a/" + x)(1)` — after which the
   outer value would be substituted into the inner body, which is an address no call site can produce. With
   no function-like node in the expression there is nothing that can rebind the name: `let` and `var` cannot
   appear in an expression, so the parameter is the only binder of its name in the whole subtree. That makes
   the substitution correct BY CONSTRUCTION rather than by a scope walk this fold does not carry. */
function hasNestedFunction(node) {
  let found = false;
  walk(node, (n) => { if (!found && FN_LIKE.has(n.type)) found = true; });
  return found;
}
function singleParamFn(node, binds, env) {
  const r = returnExprFnOf(node, binds, env);
  if (!r || r.refused) return null;
  if (r.fn.params.length !== 1 || r.fn.params[0].type !== "Identifier") return null;
  return r.fn;
}

/* EVERYTHING THE ORDINARY CHANNEL MAY NOT DO, IN ONE PLACE, REACHED ONLY WITH AN `env`. Returning null hands
   the node back to the folder's own switch, so a node this mode has nothing to say about is folded exactly as
   it is folded with no env at all. */
function foldEnvOnly(node, binds, depth, env) {
  switch (node.type) {
    case "Identifier":
      /* THE ENUMERATED PARAMETER. Its value is a candidate drawn from the function's OWN body, so the text
         this returns is a value the bundler really can be called with rather than one invented here. */
      if (env.vars.has(node.name)) return { text: env.vars.get(node.name), holes: 0 };
      /* THE PARAMETER OF A FUNCTION WITH ONE CALL SITE — see `collectBinds` for what makes that sound.
         Priced by the numbers `sole` carries, for the reason the inline is priced: a widening that buys
         coverage is paid for in false resolutions, and the two are read together or the trade is not being
         made. `unsettled` is counted apart from `settled` because a parameter resolved to an argument
         that is ITSELF opaque is the arm working and buying nothing, which is a different fact from the arm
         refusing. */
      if (env.sole && env.paramOf) {
        const ps = env.paramOf.get(node);
        if (!ps) return null;
        const no = (k) => { env.sole.refused.set(k, (env.sole.refused.get(k) || 0) + 1); return null; };
        const call = env.callSiteOf.get(ps.fn);
        if (!call) return no("no call site: " + (env.whyNoCall.get(ps.fn) ||
          "it is not a named value at all — it is " + (env.roleOfFn.get(ps.fn) || "in a role this pass does not name")));
        /* A CYCLE IS POSSIBLE EVEN THOUGH EACH FUNCTION HAS ONE CALL SITE — two helpers calling only each
           other are unreachable code and still a cycle in this graph — so the walk carries its own seen set
           rather than relying on the depth limit to end it. */
        if (env.sole.seen.has(ps.fn)) return no("a cyclic call graph");
        const as = call.arguments || [];
        /* A SPREAD MAKES POSITION MEANINGLESS: `f(...xs)` binds parameter i to an element nothing here
           can name, so the whole call is refused rather than any one position. */
        if (as.some((x) => x && (x.type === "SpreadElement" || x.type === "ArgumentPlaceholder")))
          return no("a spread argument");
        /* FEWER ARGUMENTS THAN PARAMETERS LEAVES THIS ONE `undefined`, which is a value the call does not
           supply, so there is nothing to fold. */
        if (ps.index >= as.length) return no("the call supplies no argument at that position");
        const seen = new Set(env.sole.seen); seen.add(ps.fn);
        const got = fold(as[ps.index], binds, depth + 1, { ...env, sole: { ...env.sole, seen } });
        if (got.holes === 0) env.sole.settled++; else env.sole.unsettled++;
        return got;
      }
      return null;
    case "MemberExpression":
    case "OptionalMemberExpression": {
      /* `MAP[id]` WITH A LITERAL KEY. The object is either written inline — which is what a minified chunk
         table is — or is a name this file already trusts to be one thing. */
      if (!node.computed) {
        /* A PROPERTY ASSIGNED EXACTLY ONCE IN THE FILE, which is where a bundler keeps its public path and
           its chunk-URL function: `o.p="/assets/webpack/"`, `p.u=id=>...`. The once-ness is the same argument
           that makes a resolved NAME foldable — a slot written in one place cannot be two things — and it
           additionally requires the OBJECT to resolve to a binding declared once in its own scope and never
           written, because a property of an object nobody can identify names nothing. */
        const m = env.slotOf.get(node);
        if (m) return deref(fold(m, binds, depth + 1, env), env);
        return null;
      }
      const key = fold(node.property, binds, depth + 1, env);
      if (key.holes !== 0) return null;
      const obj = objectLiteralOf(node.object, binds, env);
      if (!obj) return null;
      for (const q of obj.properties) {
        if (q.type !== "ObjectProperty" || q.computed) continue;
        const k = q.key.type === "Identifier" ? q.key.name
                : q.key.type === "StringLiteral" ? q.key.value
                : q.key.type === "NumericLiteral" ? String(q.key.value) : null;
        if (k === key.text) return deref(fold(q.value, binds, depth + 1, env), env);
      }
      return MISS();
    }
    case "LogicalExpression": {
      /* ONLY PAST A MISS, which is the one case a static reader can settle without knowing a runtime value:
         `undefined || x` IS `x` in every execution, so this decides nothing the program had a choice about. */
      if (node.operator !== "||") return null;
      const l = fold(node.left, binds, depth + 1, env);
      if (l.miss) return fold(node.right, binds, depth + 1, env);
      if (l.holes === 0 && l.text !== "") return l;
      return null;
    }
    case "BinaryExpression": {
      /* AN EQUALITY BETWEEN TWO SETTLED OPERANDS, so the ternary chain a bundler writes its manifest as can
         be DECIDED per candidate instead of collapsing to a hole. Compared as TEXT because that is what the
         fold produces, and `9016===e` with the candidate `9016` is the only shape this has to answer. */
      if (node.operator !== "===" && node.operator !== "==" &&
          node.operator !== "!==" && node.operator !== "!=") return null;
      const a = fold(node.left, binds, depth + 1, env), b = fold(node.right, binds, depth + 1, env);
      if (a.holes !== 0 || b.holes !== 0) return null;
      const eq = a.text === b.text;
      /* THE MARKER CARRIES A HOLE AND ITS ONLY READER NEVER READS ITS TEXT, which is what keeps this arm
         from INVENTING an address the day a channel wider than the manifest folds with an env. A decided
         comparison is a CONTROL-FLOW fact and not a byte of a URL; returning it hole-free made `"/a/" + (x
         === y)` fold to `/a/` with nothing missing, and a row with no holes is reported as a COMPLETE
         address. `ConditionalExpression` below consumes `.cmp` and never `.text`, so the hole costs the
         manifest channel nothing — asserted by the manifest's own totals being byte-identical across this
         change. */
      return { text: "{?}", holes: 1, cmp: node.operator[0] === "!" ? !eq : eq,
               why: ["an operator this fold does not evaluate"] };
    }
    case "ConditionalExpression": {
      const t = fold(node.test, binds, depth + 1, env);
      if (t.cmp === undefined) return null;
      return fold(t.cmp ? node.consequent : node.alternate, binds, depth + 1, env);
    }
    case "CallExpression":
    case "OptionalCallExpression": {
      /* INLINING THE ONE APPLICATION THIS ROW IS ABOUT. `env.app` is that call node and there is exactly one
         of it per row — a composition holding two applications is REFUSED by the scanner rather than guessed
         at, because two unknown parameters make the address set a product of two domains and nothing here
         has established the two are ever indexed together. */
      if (node === env.app) {
        const fn = env.fn;
        const inner = new Map(env.vars);
        inner.set(fn.params[0].name, env.candidate);
        return fold(returnExprOf(fn), binds, depth + 1, { ...env, vars: inner });
      }
      /* CROSSING THE FUNCTION BOUNDARY AT A CALL WHOSE ARGUMENTS THIS FOLD ALREADY SETTLED — the shape the
         per-argument fold STOPPED at, and the largest thing it stopped at after a parameter. It is STRICTLY
         SOUNDER than the manifest channel's enumeration above rather than a wider version of it: the
         manifest binds a candidate DRAWN FROM THE FUNCTION'S OWN BODY, and this binds the value the call
         site actually passes, so there is no "a value the bundler really can be called with" argument to
         make — it is the value it IS called with.
         THE ARITY IS EXACT AND NOT A MINIMUM. `f(a)` on a two-parameter function leaves the second
         `undefined`, and a body that concatenates it would produce `/a/undefined`, which is a string the
         program really does build and NOT an address anybody serves; refusing the shape is the floor
         direction. `f(a, b)` on a one-parameter function is sound in the language and is refused too,
         because admitting it buys a shape nothing in this corpus needed and widens what has to be argued.
         THE VARS MAP IS FRESH AND NOT INHERITED, which is the one place a nested inline could invent. The
         callee's body is a different scope, so a name it shares with the CALLER's parameter means whatever
         that name means THERE — carrying the caller's binding inward would substitute a value the callee
         never receives. Starting empty yields a HOLE in that case, which is the safe answer. */
      if (!env.inline) return null;
      const no = (k) => { env.inline.refused.set(k, (env.inline.refused.get(k) || 0) + 1); return null; };
      const r = returnExprFnOf(node.callee, binds, env);
      /* THE TWO FLOOR REASONS ARE COUNTED AND NOT DESCRIBED, because they are the size of what this inline
         cannot see and a floor stated without one is read as a total. A minified member callee — `o.f(x)` —
         is the dominant one BY CONSTRUCTION rather than by accident: a bundler passes its own runtime into
         every module as a PARAMETER, so the slot map keyed on the object's binding cannot join a module's
         read of `n.u` to the runtime's write of it, and no scope-correct resolution can. */
      if (!r) return no("callee-unresolved:" + (node.callee ? node.callee.type : "none"));
      if (r.refused) return no(r.refused);
      const fn = r.fn;
      if (env.inline.seen.has(fn)) return no("recursive");
      if (node.arguments.length !== fn.params.length) return no("arity-mismatch");
      if (!fn.params.every((p) => p.type === "Identifier")) return no("param-is-a-pattern");
      const expr = returnExprOf(fn);
      if (hasNestedFunction(expr)) return no("nested-fn");
      const inner = new Map();
      for (let i = 0; i < fn.params.length; i++) {
        const av = fold(node.arguments[i], binds, depth + 1, env);
        if (av.holes !== 0) return no("argument-not-settled");
        inner.set(fn.params[i].name, av.text);
      }
      const seen = new Set(env.inline.seen); seen.add(fn);
      const got = fold(expr, binds, depth + 1,
                       { ...env, vars: inner, inline: { ...env.inline, seen } });
      if (got.holes === 0) env.inline.settled++;
      return got;
    }
    default:
      return null;
  }
}

/* ── ONE FILE ─────────────────────────────────────────────────────────────────────────────────────────────
   Two passes over one AST. The first collects the bindings that are safe to fold and the guard nesting; the
   second reads the doors. They are two passes rather than one because a bundle names a constant AFTER using
   it as often as before, and a single forward pass would fold a declaration's own order into the answer. */

function walk(root, enter, leave) {
  const stack = [{ node: root, entered: false }];
  while (stack.length) {
    const fr = stack[stack.length - 1];
    if (!fr.entered) {
      fr.entered = true;
      enter(fr.node);
      const keys = VISITOR_KEYS[fr.node.type] || [];
      const kids = [];
      for (const k of keys) {
        const v = fr.node[k];
        if (Array.isArray(v)) { for (const c of v) if (c && typeof c.type === "string") kids.push(c); }
        else if (v && typeof v.type === "string") kids.push(v);
      }
      for (let i = kids.length - 1; i >= 0; i--) stack.push({ node: kids[i], entered: false });
    } else {
      stack.pop();
      if (leave) leave(fr.node);
    }
  }
}

/* WHICH BINDING A REFERENCE MEANS — A SCOPE PRE-PASS, KEYED BY NODE IDENTITY.
   THIS USED TO BE A FILE-WIDE COUNT AND THE ARGUMENT FOR IT IS KEPT HERE BECAUSE A READER WILL RE-DERIVE IT.
   It read: a name is foldable only if the WHOLE FILE binds it once and assigns it never; that is stronger
   than scope-correctness and was chosen for it, since without a scope graph a name bound twice could be
   folded across a shadow and a wrong fold INVENTS an address — the one failure this file may not have. Every
   clause of that is true and the conclusion it licensed is not, because the premise it rests on is the
   ABSENCE of a scope graph rather than any property of a name. A scope-correct binder is not a weaker test
   than file-wide bound-once; it is the test the LANGUAGE makes. `const B="/a"` outside a function and
   `const B="/b"` inside one are two bindings and a reference names exactly one of them, so resolving the
   reference cannot fold across a shadow — there is no shadow left to fold across.
   SO IT IS STRICTLY BOTH MORE PRECISE AND WIDER, AND THE WIDER HALF IS THE ONE THIS FILE EXISTS TO BUY. The
   file-wide test refuses a name bound twice wherever the second binding is, and a minified bundle's runtime
   binds one letter a dozen times in a dozen nested functions that enclose neither the declaration nor the
   use. MEASURED on one webpack-4 runtime in the corpus this ran against: its chunk-URL function `t` and the
   object `o` holding its public path are each declared at the runtime's own scope AND bound again by a
   `var d,t,o=a[0]` inside one nested function, which also assigns `t` — so the file-wide test refused the
   function for its bind count AND for a reassignment that writes a different binding entirely, and the whole
   chunk manifest of that site was invisible. This is that site's residual built.
   THE MAPS ARE KEYED BY THE REFERENCE NODE AND NOT BY ITS NAME, which is what makes the change cheap: the
   fold already receives the AST node, so no scope has to be threaded through it and no call site grows an
   argument. A lookup that misses is a refusal, exactly as a missing name was.
   PLACEMENT IS DELIBERATELY THE WIDER OF TWO READINGS WHEREVER THE LANGUAGE HAS TWO. A `var` and a function
   declaration are placed in the nearest FUNCTION scope; `let`, `const` and a class in the nearest BLOCK. A
   function declaration inside a block is block-scoped in strict mode and hoisted to the function scope by
   Annex B in sloppy mode, and this pass cannot always know which — so it takes the FUNCTION scope, which
   makes two such declarations of one name collide and be REFUSED where block placement would have folded
   each. Wider placement can only ever merge bindings, and merging refuses; narrower placement could split
   one binding into two and fold. The direction is the one that declines.
   A `with` BODY IS REFUSED OUTRIGHT because inside one a bare name may denote a PROPERTY of the object and no
   lexical resolution answers for it. Direct `eval` can add a `var` to its enclosing function scope and this
   pass cannot see it; that exposure is unchanged by this diff — the file-wide count could not see an eval'd
   binding either, since it is not in the file's text at all — and it is declared here rather than left to be
   inferred.
   `count` SURVIVES AS A FILE-WIDE NAME COUNT AND ANSWERS A DIFFERENT QUESTION. The ceiling column prints
   "times that name is bound in its own FILE" as a statement about what a minifier did, and that is a fact
   about the text rather than about any one reference — so it is computed exactly as it was, over the same
   over-binding pattern walk, and this diff moves it by nothing. */
const FN_LIKE = new Set(["FunctionDeclaration", "FunctionExpression", "ArrowFunctionExpression",
                         "ObjectMethod", "ClassMethod", "ClassPrivateMethod"]);
const VAR_SCOPES = new Set([...FN_LIKE, "Program", "StaticBlock"]);
const BLOCK_SCOPES = new Set(["BlockStatement", "ForStatement", "ForInStatement", "ForOfStatement",
                              "SwitchStatement", "CatchClause", "ClassDeclaration", "ClassExpression",
                              "WithStatement"]);
/* ASSERTED AGAINST @babel/types RATHER THAN TRUSTED, for the reason `FN_SCOPES` is: a Babel release that
   renamed one of these would leave its bindings placed in an enclosing scope, which MERGES nothing and
   SPLITS nothing visibly — it would quietly widen what folds, which is the direction that invents. */
for (const t of [...VAR_SCOPES, ...BLOCK_SCOPES])
  if (!(t in VISITOR_KEYS))
    die(`the scope pre-pass names the node kind ${t}, which this @babel/types does not have — a binding it ` +
        `should hold would be placed in an enclosing scope instead, which widens what this file folds ` +
        `without saying so, and a fold that is wrong INVENTS an address.`);

function collectBinds(ast) {
  /* PHASE 1 — one walk: open and close scopes, place every declaration, and record every reference with the
     scope it stands in. References cannot be resolved here because a bundle names a constant AFTER using it
     at least as often as before, which is the same reason this file has always been two passes. */
  let nextId = 0;
  const mkScope = (parent, isVar, inWith) => ({ id: nextId++, parent, isVar, inWith, decls: new Map() });
  const rootScope = mkScope(null, true, false);
  let cur = rootScope;
  const open = [];
  const refs = [];            // { n: Identifier, s: scope }            every identifier, reference or not
  const members = [];         // { n: MemberExpression, s: scope }      every `obj.prop` shape
  const asgTargets = [];      // { n: Identifier, s: scope }            `x = v` and `x++`
  const memberWrites = [];    // { obj, prop, value, plain, s }
  const memberComputed = [];  // { obj, s }                            `obj[k] = v`, which may be any slot
  const count = new Map();    // FILE-WIDE name -> binding occurrences; the ceiling column's own question
  /* WHICH IDENTIFIER NODES ARE NOT REFERENCES AT ALL, AND WHY THE SET HAS TO BE EXPLICIT. The reference
     COUNT below decides whether a function can be entered from anywhere but one call, so a node counted
     wrongly matters in BOTH directions and they are not alike: an over-count refuses a sound closure, which
     is the floor, and an UNDER-count admits an unsound one, which would hand a parameter a value some other
     call site never passes. Every member here is a node the language resolves to no binding — a
     declaration's own name, a plain parameter's name, a non-computed member PROPERTY, a non-computed
     object or class KEY, and a label. Everything this walker cannot classify stays counted, which is the
     over-counting direction on purpose.
     THE MEMBER-PROPERTY MEMBER IS THE LOAD-BEARING ONE AND IT WAS MEASURED, NOT ASSUMED: the slack bundle
     declares `function sendBeacon(r,n)` and spells `navigator.sendBeacon` three times, so a count that
     read a property name as a reference would put that binding at four references and refuse the one closure
     on this corpus that recovers a real endpoint path. */
  const notRef = new Set();
  const paramSlot = new Map();      // the decl object of a PLAIN parameter -> { fn, index }
  const calleeOf = new Map();       // a call's callee NODE -> that call
  const declOfFn = new Map();       // a function node -> the binding it is the value of, where it has one
  const usesArguments = new Set();  // fn nodes under which the name `arguments` appears at all
  const fnStackCB = [];
  /* WHAT SYNTACTIC ROLE AN UNNAMED FUNCTION OCCUPIES, because `it is not a named value at all` is a count
     over at least three populations that take OPPOSITE work and no reading of one number separates them.
     A function that is the VALUE OF A MEMBER WRITE is reachable by the same once-written question `slotOf`
     already answers for a property, one level over from the reference count. A CLASS or OBJECT METHOD is
     reached through a RECEIVER, so naming its arguments needs the receiver's type and that is a recogniser
     §RUN-DON'T-MATCH forbids. A CALLBACK ARGUMENT has its parameter bound by the CALLEE's own semantics —
     a resolution value, an array element, an Event — so there is no argument in this file to read at all and
     no parse ever recovers it. Carrying the role INTO the refusal string is what makes the existing block
     partition them, rather than adding a second count that could disagree with the first.
     THE ROLE IS RECORDED FROM THE PARENT SIDE AND WITHOUT A PARENT STACK: every role here is a property of
     a node kind that is a small fraction of the tree, so the question is asked once when that parent is
     entered rather than of every node on the way down. A method needs no parent, being its own node kind. */
  const roleOfFn = new Map();
  /* WHICH CALLEE A CALLBACK WAS HANDED TO, recorded in the SAME STATEMENT as the role so the two cannot
     drift into disagreeing about one function. The SPELLING is recorded here and CLASSIFIED nowhere near
     here: `collectBinds` stays free of any notion of what a platform name means, and `VALUE_SUPPLIERS` is
     the one place that decides. Two classifications of one thing is the shape that drifts; a spelling and
     its classification in two layers is not. */
  const cbVia = new Map();
  const role = (child, what) => { if (child && FN_LIKE.has(child.type) && !roleOfFn.has(child)) roleOfFn.set(child, what); };

  const declare = (scope, name, initNode, fnNode) => {
    let d = scope.decls.get(name);
    if (!d) { d = { n: 0, init: null, fn: null, reassigned: false }; scope.decls.set(name, d); }
    d.n++;
    if (initNode) d.init = initNode;
    if (fnNode) d.fn = fnNode;
    count.set(name, (count.get(name) || 0) + 1);
  };
  const varScopeOf = (s) => { let x = s; while (x && !x.isVar) x = x.parent; return x || rootScope; };
  /* A PATTERN BINDS EVERY IDENTIFIER UNDER IT AND NONE OF THEM GETS AN INITIALIZER, which over-binds — a
     default value's own free names are counted too. That is the behaviour the ceiling column's number was
     measured with and it is kept unchanged: over-binding adds occurrences, and an extra occurrence can only
     make a name collide and be refused. */
  const declPattern = (scope, pat) => {
    if (!pat) return;
    if (pat.type === "Identifier") { declare(scope, pat.name, null, null); return; }
    walk(pat, (n) => { if (n.type === "Identifier") declare(scope, n.name, null, null); });
  };

  walk(ast, (n) => {
    /* DECLARATIONS THAT BELONG TO THE SCOPE THIS NODE SITS IN are placed BEFORE the node's own scope opens. */
    switch (n.type) {
      case "VariableDeclaration": {
        /* THE KIND IS ONLY LEGIBLE HERE, which is why this is handled at the declaration and not at the
           declarator: `var` goes to the nearest function scope and `let`/`const` stay in this block. */
        const target = n.kind === "var" ? varScopeOf(cur) : cur;
        for (const d of n.declarations || []) {
          if (!d || d.type !== "VariableDeclarator") continue;
          if (d.id.type === "Identifier") {
            declare(target, d.id.name, d.init || null, null);
            notRef.add(d.id);
            if (d.init && FN_LIKE.has(d.init.type)) declOfFn.set(d.init, target.decls.get(d.id.name));
          } else declPattern(target, d.id);
        }
        break;
      }
      case "FunctionDeclaration":
        if (n.id) {
          declare(varScopeOf(cur), n.id.name, null, n);
          notRef.add(n.id);
          declOfFn.set(n, varScopeOf(cur).decls.get(n.id.name));
        }
        break;
      case "ClassDeclaration":
        if (n.id) { declare(cur, n.id.name, null, null); notRef.add(n.id); }
        break;
      case "ImportSpecifier": case "ImportDefaultSpecifier": case "ImportNamespaceSpecifier":
        declare(rootScope, n.local.name, null, null); notRef.add(n.local);
        break;
      case "AssignmentExpression": {
        if (n.left.type === "Identifier") asgTargets.push({ n: n.left, s: cur });
        const k = memberKey(n.left);
        if (k) memberWrites.push({ obj: n.left.object, prop: n.left.property.name, value: n.right,
                                   plain: n.operator === "=", s: cur });
        else if ((n.left.type === "MemberExpression" || n.left.type === "OptionalMemberExpression") &&
                 n.left.computed && n.left.object.type === "Identifier")
          memberComputed.push({ obj: n.left.object, s: cur });
        break;
      }
      case "UpdateExpression": {
        if (n.argument.type === "Identifier") asgTargets.push({ n: n.argument, s: cur });
        const k = memberKey(n.argument);
        if (k) memberWrites.push({ obj: n.argument.object, prop: n.argument.property.name, value: null,
                                   plain: false, s: cur });
        break;
      }
      default: break;
    }
    /* THE NON-REFERENCE IDENTIFIER POSITIONS, the callee map the unique-call-site question reads, and the
       one construct that defeats the whole closure. `arguments[0] = v` in sloppy mode ALIASES a parameter
       and leaves no write any of these passes can see, so a function mentioning the name at all is refused
       rather than reasoned about — and the mark is put on every function on the stack because an arrow has
       no `arguments` of its own and means its enclosing function's. */
    if ((n.type === "MemberExpression" || n.type === "OptionalMemberExpression") &&
        !n.computed && n.property.type === "Identifier") notRef.add(n.property);
    else if ((n.type === "ObjectProperty" || n.type === "ObjectMethod" || n.type === "ClassMethod" ||
              n.type === "ClassProperty" || n.type === "ClassPrivateMethod") &&
             !n.computed && n.key && n.key.type === "Identifier") notRef.add(n.key);
    else if (n.type === "LabeledStatement" && n.label) notRef.add(n.label);
    else if ((n.type === "BreakStatement" || n.type === "ContinueStatement") && n.label) notRef.add(n.label);
    else if ((n.type === "CallExpression" || n.type === "OptionalCallExpression") && n.callee)
      calleeOf.set(n.callee, n);
    if (n.type === "Identifier" && n.name === "arguments") for (const f of fnStackCB) usesArguments.add(f);
    switch (n.type) {
      case "AssignmentExpression":
        role(n.right, n.left && (n.left.type === "MemberExpression" || n.left.type === "OptionalMemberExpression")
          ? (n.left.computed ? "the value of a COMPUTED member write" : "the value of a member write")
          : "the value of an identifier write");
        break;
      case "ObjectProperty": role(n.value, "an object-literal property value"); break;
      case "ClassProperty": role(n.value, "a class field initialiser"); break;
      case "VariableDeclarator": role(n.init, "a variable initialiser"); break;
      case "ReturnStatement": role(n.argument, "a returned value"); break;
      case "ArrayExpression": for (const e of n.elements || []) role(e, "an array element"); break;
      case "ConditionalExpression": role(n.consequent, "a ternary arm"); role(n.alternate, "a ternary arm"); break;
      case "LogicalExpression": role(n.left, "a logical arm"); role(n.right, "a logical arm"); break;
      case "SequenceExpression": for (const e of n.expressions || []) role(e, "a sequence element"); break;
      case "CallExpression": case "OptionalCallExpression": case "NewExpression": {
        const via = n.callee && n.callee.type === "Identifier" ? n.callee.name
          : n.callee && (n.callee.type === "MemberExpression" || n.callee.type === "OptionalMemberExpression") &&
            !n.callee.computed && n.callee.property.type === "Identifier" ? n.callee.property.name : null;
        for (const x of n.arguments || []) {
          const had = roleOfFn.has(x);
          role(x, "a CALLBACK argument — its parameter is bound by the callee's own semantics");
          /* MEMBERSHIP SAYS `THIS IS A CALLBACK` AND THE VALUE SAYS `AND THIS IS ITS SUPPLIER, OR I COULD
             NOT SPELL ONE`. Two facts in one map because they are about one key and were set in one
             statement; two maps could disagree about whether a function is a callback at all. A callee this
             pass cannot spell — `t[k](fn)`, a call returning a call — is recorded with NO spelling rather
             than left out, because leaving it out reads downstream as "an ordinary parameter its own caller
             supplies", which is the flattering direction: it attributes to app code a value some platform or
             library callee will in fact bind. */
          if (!had && roleOfFn.has(x)) cbVia.set(x, via || null);
        }
        break;
      }
      default: break;
    }
    if (n.type === "ClassMethod" || n.type === "ClassPrivateMethod") roleOfFn.set(n, "a CLASS METHOD — reached through a receiver");
    else if (n.type === "ObjectMethod") roleOfFn.set(n, "an OBJECT METHOD — reached through a receiver");
    /* THEN THE NODE'S OWN SCOPE, AND WHAT BINDS INSIDE IT. */
    if (VAR_SCOPES.has(n.type) || BLOCK_SCOPES.has(n.type)) {
      open.push(cur);
      cur = mkScope(cur, VAR_SCOPES.has(n.type), cur.inWith || n.type === "WithStatement");
      if (FN_LIKE.has(n.type)) {
        fnStackCB.push(n);
        /* A NAMED FUNCTION EXPRESSION'S OWN NAME IS VISIBLE INSIDE IT AND NOWHERE ELSE. */
        if (n.type === "FunctionExpression" && n.id) { declare(cur, n.id.name, null, n); notRef.add(n.id); }
        for (let i = 0; i < (n.params || []).length; i++) {
          const q = n.params[i];
          declPattern(cur, q);
          /* THE PARAMETER SLOT IS RECORDED ONLY FOR A PLAIN IDENTIFIER. A pattern, a default and a rest each
             stand for something other than "argument i", so none of them is recorded and the arm that reads
             this map has nothing to answer with — which is the refusal and not a gap. */
          if (q && q.type === "Identifier") {
            notRef.add(q);
            paramSlot.set(cur.decls.get(q.name), { fn: n, index: i });
          }
        }
      } else if (n.type === "CatchClause") declPattern(cur, n.param);
      else if (n.type === "ClassExpression" && n.id) declare(cur, n.id.name, null, null);
    }
    if (n.type === "Identifier") refs.push({ n, s: cur });
    else if (n.type === "MemberExpression" || n.type === "OptionalMemberExpression") members.push({ n, s: cur });
  }, (n) => {
    if (FN_LIKE.has(n.type)) fnStackCB.pop();
    if (VAR_SCOPES.has(n.type) || BLOCK_SCOPES.has(n.type)) cur = open.pop();
  });

  /* PHASE 2 — resolution. A reference means the binding of the nearest enclosing scope that declares its
     name, which is what the language says and is the whole of this pass's claim. */
  const resolve = (scope, name) => {
    for (let s = scope; s; s = s.parent) { const d = s.decls.get(name); if (d) return { scope: s, d }; }
    return null;
  };
  /* A WRITE MARKS THE BINDING IT RESOLVES TO, NOT EVERY BINDING OF ITS NAME. That distinction is the whole
     of the gitlab case: `t=o[s]` inside a nested function writes that function's own `var t` and says
     nothing whatever about the `function t(e)` one scope out. */
  const freeAssigned = new Set();
  for (const { n, s } of asgTargets) {
    if (s.inWith) { freeAssigned.add(n.name); continue; }
    const r = resolve(s, n.name);
    if (r) r.d.reassigned = true; else freeAssigned.add(n.name);
  }

  /* PHASE 3 — the node-keyed answers. */
  const binds = new Map();     // reference node -> { node: its binding's initializer }
  const fnDeclOf = new Map();  // reference node -> the FunctionDeclaration its binding IS
  const freeRef = new Set();   // reference node that resolves to NO binding and whose name is never
                               // assigned free either, so nothing in this file can have made it anything
  for (const { n, s } of refs) {
    if (s.inWith) continue;
    const r = resolve(s, n.name);
    if (!r) { if (!freeAssigned.has(n.name)) freeRef.add(n); continue; }
    const d = r.d;
    if (d.n !== 1 || d.reassigned) continue;
    if (d.init) binds.set(n, { node: d.init });
    else if (d.fn) fnDeclOf.set(n, d.fn);
  }
  /* THE SLOTS A SINGLE ASSIGNMENT SETTLES, KEYED ON THE OBJECT'S BINDING RATHER THAN ON ITS NAME. Three
     conditions and each one still refuses a real corpus shape: the slot is written exactly once with a plain
     `=`; the OBJECT resolves to a binding declared once in its own scope and never written, so the slot
     belongs to one object; and nothing writes that object through a COMPUTED property, which could be this
     slot under another spelling. What has changed is only WHICH `o` is being asked about. */
  const slotKey = (objNode, s) => {
    if (!objNode || objNode.type !== "Identifier" || s.inWith) return null;
    const r = resolve(s, objNode.name);
    if (!r || r.d.n !== 1 || r.d.reassigned) return null;
    return r.scope.id + "\u0000" + objNode.name;
  };
  const slotWrites = new Map(), slotPoison = new Set();
  for (const w of memberComputed) { const k = slotKey(w.obj, w.s); if (k) slotPoison.add(k + "\u0000*"); }
  for (const w of memberWrites) {
    const k = slotKey(w.obj, w.s);
    if (!k) continue;
    const sk = k + "\u0000" + w.prop;
    if (!w.plain) { slotPoison.add(sk); continue; }
    const had = slotWrites.get(sk);
    if (had) had.n++; else slotWrites.set(sk, { n: 1, node: w.value });
  }
  const slotOf = new Map();    // `obj.prop` READ node -> the single value written to that slot
  for (const { n, s } of members) {
    if (n.computed || n.object.type !== "Identifier" || n.property.type !== "Identifier") continue;
    const k = slotKey(n.object, s);
    if (!k || slotPoison.has(k + "\u0000*")) continue;
    const sk = k + "\u0000" + n.property.name;
    if (slotPoison.has(sk)) continue;
    const w = slotWrites.get(sk);
    if (!w || w.n !== 1) continue;
    slotOf.set(n, w.node);
  }
  /* ── THE UNIQUE CALL SITE, AND THE PARAMETER IT DETERMINES ──────────────────────────────────────────────
     WHAT THIS ANSWERS. The per-argument fold stops at a function boundary in the CALLER->CALLEE direction
     too: a door written inside a helper reads its address from a PARAMETER, which has no initializer and no
     slot, so such a row is opaque however completely the caller spelled the address. This is the other
     direction of the boundary the inline already crosses, and it was this file's own named residual.
     THE SOUNDNESS ARGUMENT IS THE ONCE-WRITTEN SLOT'S AND IT IS NOT "CALLED ONCE". A function whose binding
     is referenced exactly once in the whole file, with that one reference standing in CALLEE position, can
     be entered only through that call — so every invocation binds parameter i to the fold of THAT ONE
     ARGUMENT EXPRESSION. How many times it is invoked is irrelevant, because the argument is the same node
     each time and its fold is a sound over-approximation of what that node evaluates to, holes included.
     That is why a call inside a loop, or inside another helper called a thousand times, costs this nothing.
     WHAT A SINGLE REFERENCE EXCLUDES, and each of these is a real corpus shape: an export (`export {f}`,
     `m.exports.f = f`) is a second reference; `f.call`/`f.apply`/`f.bind` puts the reference in an
     OBJECT position rather than a callee one; `new f()` is a NewExpression and is not admitted; an alias
     (`var g = f`) is a second reference; and a recursive function references its own name, so it can never
     qualify and needs no separate guard.
     WHAT IT INHERITS RATHER THAN GUARDS, STATED SO THE TWO ARE NOT CONFUSED. A reference inside `with` IS
     counted here even though phase 3 refuses to resolve it, because an uncounted call site is the unsound
     direction while an over-counted one merely refuses. `eval` is NOT guarded, and that is the same
     assumption `binds` and `slotOf` have always made — a file that rewrites its own bindings through
     `eval` defeats every answer this pass produces, so guarding one arm alone would be a second standard
     rather than a stronger one. */
  const refCount = new Map();  // decl object -> how many identifier nodes resolve to it
  const refFirst = new Map();  // decl object -> the first such node
  for (const { n, s: sc } of refs) {
    if (notRef.has(n)) continue;
    const r = resolve(sc, n.name);
    if (!r) continue;
    refCount.set(r.d, (refCount.get(r.d) || 0) + 1);
    if (!refFirst.has(r.d)) refFirst.set(r.d, n);
  }
  const callSiteOf = new Map();  // fn node -> the one CallExpression that can enter it
  /* AN IMMEDIATELY-INVOKED FUNCTION NEEDS NO REFERENCE COUNT AT ALL: the function expression IS the callee,
     so no name exists anywhere for anything else to reach it through. */
  for (const [node, call] of calleeOf)
    if (node.type === "FunctionExpression" || node.type === "ArrowFunctionExpression") callSiteOf.set(node, call);
  for (const [d, c] of refCount) {
    if (c !== 1 || d.reassigned) continue;
    const fn = d.fn || (d.init && FN_LIKE.has(d.init.type) ? d.init : null);
    if (!fn) continue;
    const call = calleeOf.get(refFirst.get(d));
    if (call) callSiteOf.set(fn, call);
  }
  /* WHY A FUNCTION HAS NO UNIQUE CALL SITE, NAMED AS A STRUCTURE RATHER THAN COUNTED AS ONE NUMBER. The
     refusal reasons partition differently and take different work: a function REFERENCED many times is a
     shared helper and no call-site analysis will ever settle its parameters, while a function that is not a
     named value at all — an object-literal property, a callback argument, a returned closure — is the
     bundler's module shape and cannot be reached by any reference count, because there is no reference. */
  const whyNoCall = new Map();
  for (const [fn, d] of declOfFn) {
    if (callSiteOf.has(fn)) continue;
    const c = refCount.get(d) || 0;
    whyNoCall.set(fn, d.reassigned ? "its binding is reassigned"
      : c === 0 ? "it is named and never referenced"
      : c > 1 ? "it is referenced " + (c > 4 ? "5 or more" : String(c)) + " times — a shared helper"
      : "its one reference is not a callee");
  }
  const paramOf = new Map();   // reference node -> { fn, index } for a plain parameter nothing rewrites
  for (const { n, s: sc } of refs) {
    if (notRef.has(n) || sc.inWith) continue;
    const r = resolve(sc, n.name);
    if (!r || r.d.n !== 1 || r.d.reassigned) continue;
    const ps = paramSlot.get(r.d);
    if (ps && !usesArguments.has(ps.fn)) paramOf.set(n, ps);
  }
  return { binds, fnDeclOf, slotOf, count, freeRef, paramOf, callSiteOf, whyNoCall, roleOfFn, cbVia };
}

/* ── THE CHUNK MANIFEST ───────────────────────────────────────────────────────────────────────────────────
   WHAT THIS ANSWERS AND WHY IT IS A BAND AND NOT A DOOR. A bundler that compiled `import()` away emits its
   chunk addresses as a MAP and a PUBLIC-PATH literal, composes them in a one-parameter function, and hands
   the result to an injected `<script>`. Every one of those addresses is in the file as plain literal text, so
   a PARSE has them and the site channel recovered NONE of them — the composition crosses a function boundary
   and the per-argument fold stops at the call. That was this file's own named residual and this is it built.
   IT IS NOT A REQUEST COUNT AND MUST NEVER BE QUOTED AS ONE. A manifest names every chunk the bundle COULD
   load; one run loads a few. So these rows are ADDRESSES RECOVERED FROM THE TEXT, reported beside the door
   totals and summed into none of them, in the same relationship the blind-spot band already has to a door.
   THE ENUMERATION IS OVER A DOMAIN THE FUNCTION ITSELF NAMES, WHICH IS WHAT KEEPS IT FROM INVENTING. The
   candidates are the KEYS of the object literals the body indexes with its own parameter and the LITERALS the
   body compares that parameter against — nothing else. For each candidate the whole composition is folded
   with the parameter bound to it, and a candidate whose fold leaves a hole is DROPPED. So every address
   emitted is one the function demonstrably returns for an input the function itself mentions; the set is the
   image of the enumerated domain and never a guess about the domain's extent.
   THE CORRELATION IS THE PART THAT WOULD BE WRONG IF IT WERE DONE THE OBVIOUS WAY. A chunk-URL function
   indexes TWO maps with ONE parameter — a name map and a hash map — so enumerating each map independently
   would emit the product of two domains and nearly every member of it would be an address that does not
   exist. Binding the parameter ONCE per candidate and folding the whole expression is what keeps the two
   lookups at the same point of the domain.
   IT IS KEYED ON THE EXPRESSION AND ON NO BUNDLER'S NAME, which CLAUDE.md's RUN-DON'T-MATCH rule requires:
   nothing here reads a runtime's identifier, a chunk-file naming convention or a public-path spelling. What
   it keys on is a single-parameter function applied to something, indexed by its own parameter — a shape,
   which is why it finds the webpack-4 and the webpack-5 form with one rule and would find a third. */
/* WHAT A COMPOSITION ROOT IS, AND WHY A CALL IS NOT ONE UNLESS IT IS THE APPLICATION ITSELF. A minified
   bundle is ONE top-level call — `!function(e){...}([...])` — so a rule that treated any CallExpression
   CONTAINING an application as a composition made the whole FILE the first candidate, found dozens of
   applications in it, refused it for holding two, and descended no further. Measured before the fix: 1383
   refusals and ZERO rows at every runtime this channel was built for. A string composition is a `+` or a
   template; a call is a root only when it IS the one application, which is the `el.src = t(id)` shape. */
const isStringComposer = (n) => (n.type === "BinaryExpression" && n.operator === "+") ||
                                n.type === "TemplateLiteral";

function applicationsIn(node, binds, env, out) {
  if (!node || typeof node.type !== "string") return;
  if ((node.type === "CallExpression" || node.type === "OptionalCallExpression") &&
      node.arguments.length === 1) {
    const fn = singleParamFn(node.callee, binds, env);
    if (fn) out.push({ app: node, fn });
  }
  for (const k of VISITOR_KEYS[node.type] || []) {
    const v = node[k];
    if (Array.isArray(v)) { for (const c of v) applicationsIn(c, binds, env, out); }
    else applicationsIn(v, binds, env, out);
  }
}

/* THE CANDIDATE DOMAIN, READ OUT OF THE FUNCTION'S OWN BODY AND NOWHERE ELSE. */
function candidateDomain(fn, binds, env) {
  const param = fn.params[0].name;
  const body = fn.body.type === "BlockStatement" ? fn.body.body[0].argument : fn.body;
  const out = new Set();
  const lit = (n) => n && (n.type === "StringLiteral" ? n.value
                         : n.type === "NumericLiteral" ? String(n.value) : null);
  const visit = (n) => {
    if (!n || typeof n.type !== "string") return;
    if ((n.type === "MemberExpression" || n.type === "OptionalMemberExpression") && n.computed &&
        n.property.type === "Identifier" && n.property.name === param) {
      const obj = objectLiteralOf(n.object, binds, env);
      if (obj) for (const q of obj.properties) {
        if (q.type !== "ObjectProperty" || q.computed) continue;
        const k = q.key.type === "Identifier" ? q.key.name
                : q.key.type === "StringLiteral" ? q.key.value
                : q.key.type === "NumericLiteral" ? String(q.key.value) : null;
        if (k !== null) out.add(k);
      }
    }
    if (n.type === "BinaryExpression" && (n.operator === "===" || n.operator === "==")) {
      if (n.left.type === "Identifier" && n.left.name === param) { const v = lit(n.right); if (v !== null) out.add(v); }
      if (n.right.type === "Identifier" && n.right.name === param) { const v = lit(n.left); if (v !== null) out.add(v); }
    }
    for (const k of VISITOR_KEYS[n.type] || []) {
      const v = n[k];
      if (Array.isArray(v)) { for (const c of v) visit(c); } else visit(v);
    }
  };
  visit(body);
  return { param, out };
}

function scanManifest(ast, binds, base, filename) {
  const rows = [];
  let refusedTwoApplications = 0;
  const descend = (node) => {
    if (!node || typeof node.type !== "string") return;
    const bare = (node.type === "CallExpression" || node.type === "OptionalCallExpression") &&
                 node.arguments.length === 1 ? singleParamFn(node.callee, binds, base) : null;
    if (isStringComposer(node) || bare) {
      const apps = [];
      if (bare) apps.push({ app: node, fn: bare });
      else applicationsIn(node, binds, base, apps);
      if (apps.length === 1) {
        const { app, fn } = apps[0];
        const dom = candidateDomain(fn, binds, base);
        if (dom.out.size) {
          const addrs = new Set(), frags = new Set();
          let dropped = 0;
          for (const cand of dom.out) {
            const r = fold(node, binds, 0, { ...base, vars: new Map(), app, fn, candidate: cand });
            if (r.holes !== 0 || r.text === "") { dropped++; continue; }
            if (looksLikeAddress(r.text)) addrs.add(r.text); else frags.add(r.text);
          }
          if (addrs.size || frags.size) {
            rows.push({ file: filename, line: node.loc ? node.loc.start.line : 0,
                        candidates: dom.out.size, dropped, addresses: [...addrs], fragments: frags.size });
            return;   /* the OUTERMOST composition owns the row; an inner one would re-report it shorter */
          }
        }
      } else if (apps.length > 1) {
        /* AND THE REFUSAL DOES NOT DESCEND, which is the half a counter alone would have got wrong. Walking
           into a refused composition finds one of its applications on its own and enumerates THAT — yielding
           an address with the other application's whole contribution missing from it, which is not a shorter
           answer but a fabricated one. The control that removes this `return` reports such a row. */
        refusedTwoApplications++;
        return;
      }
    }
    for (const k of VISITOR_KEYS[node.type] || []) {
      const v = node[k];
      if (Array.isArray(v)) { for (const c of v) descend(c); } else descend(v);
    }
  };
  descend(ast);
  return { rows, refusedTwoApplications };
}

/* GUARD DEPTH IS PROVENANCE AND NOT DECORATION. CLAUDE.md §What-the-tool-produces' proposition is "what the
   bundle CAN do but didn't", and a parse reaches a gated call site whether the gate is taken or not — which
   is the one axis on which a parse is structurally STRONGER than a run, and it must be measured rather than
   conceded or assumed. The depth is the number of enclosing tests a runtime would have to satisfy: an `if`
   or `switch` body, a `?:` arm, the right-hand side of `&&`/`||`/`??`, and a `catch`. */
const GUARDS = new Set(["IfStatement", "ConditionalExpression", "SwitchCase", "CatchClause"]);
/* EVERY NODE THAT OPENS A FUNCTION SCOPE — the population `fnDepth` counts. A class STATIC BLOCK and a
   getter/setter are in it for the same reason an ordinary method is: each is a body something has to invoke.
   A `Program` is deliberately NOT in it, because depth 0 is exactly "the module body runs this". The list is
   asserted against @babel/types rather than trusted, one line down, so a Babel release that renames a node
   kind fails loudly instead of silently reporting every call in that shape at depth 0 — which is the
   flattering direction and the one that would make a door look reachable. */
const FN_SCOPES = new Set(["FunctionDeclaration", "FunctionExpression", "ArrowFunctionExpression",
                           "ObjectMethod", "ClassMethod", "ClassPrivateMethod", "StaticBlock"]);
for (const t of FN_SCOPES)
  if (!(t in VISITOR_KEYS))
    die(`FN_SCOPES names the node kind ${t}, which this @babel/types does not have — the reach column would ` +
        `report every call inside one at function depth 0, which reads as "the module body runs this" and is ` +
        `the direction that makes a door look reachable when it is not.`);

function readFile(src, filename) {
  let ast = null, err = null;
  for (const sourceType of ["module", "script"]) {
    try { ast = parse(src, { sourceType, errorRecovery: false, plugins: [] }); err = null; break; }
    catch (e) { err = e; }
  }
  if (!ast) return { parsed: false, error: String(err && err.message || err).slice(0, 160), sites: [], pathish: new Set(), blind: [], xhrOpenSkippedNonLiteralMethod: 0,
                     globalDoor: { admitted: 0, refusedBoundName: 0, declinedNonGlobalReceiver: 0 },
                     manifest: { rows: [], refusedTwoApplications: 0 },
                     spell: new Map(), spellOther: { globalComputedDynamic: 0 },
                     inline: { settled: 0, refused: new Map() }, recvDoor: { admitted: 0, declined: 0 },
                     sole: { settled: 0, unsettled: 0, refused: new Map() }, mutFloor: { demoted: 0, rows: [] },
                     callResult: { verdict: new Map(), rows: [] } };

  const { binds, fnDeclOf, slotOf, count: bindCount, freeRef, paramOf, callSiteOf, whyNoCall, roleOfFn, cbVia } = collectBinds(ast);
  /* THE ENUMERATING FOLD'S FIXED HALF, built once per file: which binding a reference resolves to and which
     slot a member read names. The per-row half — which application, which candidate — is added at the row. */
  /* `freeRef` IS PART OF THE FOLD'S ENV AND NOT ONLY THE DOOR MATCHER'S, because the coercion arms below
     ask the same question the global door asks — is this `URL`, this `Request`, this `String` the platform's
     one — and one answer read two ways is what keeps them from disagreeing. A fold with no env cannot answer
     it and REFUSES, which is the floor direction. */
  const envBase = { fnDeclOf, slotOf, freeRef, paramOf, callSiteOf, whyNoCall, roleOfFn, cbVia };
  const manifest = scanManifest(ast, binds, envBase, filename);
  /* THE DOOR CHANNEL'S OWN ENV, AND WHY THE DOOR CHANNEL HAS ONE AT ALL. `foldEnvOnly` was reached only
     with a manifest CANDIDATE, so every arm in it that needs no candidate was being withheld from the rows
     this file's headline is about: a property written exactly once (`slotOf`), a map indexed by a literal
     key, an `||` past a key the map does not carry, and a decided comparison. Each of those is a fold a
     parse can make WITHOUT RUNNING ANYTHING and each already carries its soundness argument at its own arm —
     the mode existed to keep the manifest band ADDITIVE to the totals other lanes were pricing, which is a
     coordination reason and never a soundness one. CLAUDE.md's DOORS comment settles which of the two wins:
     "a site the parse can reach and this file was missing is a defect in this file however small the count,
     and the count moving an existing total is a fact to report as MOVED rather than a reason to leave the
     hole open."
     THE TWO CANDIDATE-DEPENDENT ARMS DECLINE THEMSELVES AND ARE NOT SWITCHED OFF BY A FLAG, which is what
     keeps this ONE folder with one mode rather than two folders free to disagree: `vars` is EMPTY so the
     parameter arm answers null for every name, and `app` is null so the manifest's inline arm answers null
     for every call. What the door channel adds is `inline`, which is the OTHER direction of the same
     boundary crossing and is priced by the three numbers it carries. */
  const inlineBudget = { seen: new Set(), settled: 0, refused: new Map() };
  const soleBudget = { seen: new Set(), settled: 0, unsettled: 0, refused: new Map() };
  /* THE FLOOR'S COUNTER AND THE ROWS IT DEMOTED. Kept on the DOOR env rather than inside `deref` so the
     rows can be named at the site that owns them: a demotion read off a global would say how many there
     are and never WHERE, and where is the whole of what makes the residual above answerable. */
  const mutFloor = { demoted: 0, rows: [] };
  const envDoor = { ...envBase, vars: new Map(), app: null, fn: null, candidate: null,
                    inline: inlineBudget, sole: soleBudget, mutFloor, callHoles: new Set() };
  const sites = [];
  const pathish = new Set();
  const blind = [];
  let xhrOpenSkippedNonLiteralMethod = 0;
  /* THE GLOBAL-REACHED DOOR'S OWN THREE NUMBERS, so the widening is read beside its price on every run. */
  const globalDoor = { admitted: 0, refusedBoundName: 0, declinedNonGlobalReceiver: 0 };
  const recvDoor = { admitted: 0, declined: 0 };
  /* A REFERENCE IS THE GLOBAL OBJECT ONLY IF IT RESOLVES TO NO BINDING AT ALL and nothing in the file
     assigns that name as a free one either. THIS USED TO ASK WHETHER THE FILE BINDS THE NAME ANYWHERE, and
     the argument is kept because a reader will re-derive it: a file that BINDS `window` can mean something
     else by it, and the UMD wrapper `(function(window){...})(window)` and the transpiler idiom
     `var self = this` both do. That is right about those two sites and wrong about every reference OUTSIDE
     them — the wrapper's parameter shadows nothing beyond its own body, and `provably the global` is a
     question about a REFERENCE rather than about a name. The scope pre-pass answers it for the reference,
     which is what `PROVABLY IS THE LOAD-BEARING WORD` in the DOORS comment always meant. */
  const provenGlobal = (o) => !!o && o.type === "Identifier" && GLOBAL_OBJECTS.has(o.name) && freeRef.has(o);
  const attached = new Set();          // node identity of URL args, so a door's own literal is not double-counted
  let guard = 0;
  const guardStack = [];
  /* WHAT ENCLOSES THE CALL — the REACH axis, kept on its own stack for the reason the guard stack is: a
     depth read off the node afterwards would need a parent chain this walker deliberately does not carry.
     It answers a different question from `guard` and the two are not substitutes. `guard` says whether a
     TEST stands in front of the call, which a parse reads whether the gate is taken or not; this says what
     has to be CALLED for the call to happen at all, which is the only thing that separates a door the
     engine rings from one it does not. */
  const fnStack = [];

  walk(ast, (n) => {
    if (GUARDS.has(n.type)) { guardStack.push(n); guard++; }
    else if (n.type === "LogicalExpression") { guardStack.push(n); guard++; }
    if (FN_SCOPES.has(n.type)) fnStack.push(n);

    /* ONE HELPER FOR BOTH SHAPES, because `window.fetch(u)` and `new self.Worker(u)` pose the identical
       question — is the thing to the left the global object — and two copies of the answer would be free to
       disagree about it. `family` is the door family the PROPERTY is looked up in, which is why a
       `member-call` door can never arrive here: the assert under DOORS keeps the two families disjoint. */
    const globalReached = (obj, prop, family) => {
      if (!DOOR_BY_NAME.has(family + ":" + prop)) return null;
      if (provenGlobal(obj)) { globalDoor.admitted++; return DOOR_BY_NAME.get(family + ":" + prop); }
      if (obj && obj.type === "Identifier" && GLOBAL_OBJECTS.has(obj.name)) globalDoor.refusedBoundName++;
      else globalDoor.declinedNonGlobalReceiver++;
      return null;
    };

    /* THE RECEIVER-QUALIFIED DOOR'S OWN TEST, AND ITS PRICE. The receiver is asked for the platform name the
       door declares, read off its OWN property (`navigator.serviceWorker.register`) or off a bare identifier
       of that name. A receiver reached some other way — `const sw = navigator.serviceWorker; sw.register(u)`
       — is DECLINED and counted rather than resolved through `binds`, because a name bound to the platform
       object and a name bound to a container are the same shape to this test and the count is what says how
       much that costs. */
    const receiverQualified = (c) => {
      const d = DOOR_BY_NAME.get("member-call-on:" + c.property.name);
      if (!d) return null;
      const o = c.object;
      const rn = o && o.type === "Identifier" ? o.name
               : o && (o.type === "MemberExpression" || o.type === "OptionalMemberExpression") &&
                 !o.computed && o.property.type === "Identifier" ? o.property.name : null;
      if (rn === d.recv) { recvDoor.admitted++; return d; }
      recvDoor.declined++;
      return null;
    };

    let door = null, args = null;
    if (n.type === "CallExpression" || n.type === "OptionalCallExpression") {
      const c = n.callee;
      if (c && c.type === "Identifier")              door = DOOR_BY_NAME.get("callee-global:" + c.name);
      else if (c && c.type === "Import")             door = DOOR_BY_NAME.get("dynamic-import:import");
      else if (c && (c.type === "MemberExpression" || c.type === "OptionalMemberExpression") &&
               !c.computed && c.property.type === "Identifier")
                                                     door = DOOR_BY_NAME.get("member-call:" + c.property.name) ||
                                                            receiverQualified(c) ||
                                                            globalReached(c.object, c.property.name, "callee-global");
      args = n.arguments;
    } else if (n.type === "NewExpression" && n.callee) {
      if (n.callee.type === "Identifier") door = DOOR_BY_NAME.get("new:" + n.callee.name);
      else if ((n.callee.type === "MemberExpression" || n.callee.type === "OptionalMemberExpression") &&
               !n.callee.computed && n.callee.property.type === "Identifier")
        door = globalReached(n.callee.object, n.callee.property.name, "new");
      args = n.arguments;
    }

    if (door && args) {
      if (door.minArgs && args.length < door.minArgs) door = null;
      /* `.open(` is XMLHttpRequest's only through a first argument that is an HTTP method. Without that
         test the row would be dominated by `window.open`, `db.open`, and every library's `open()` — which
         is the precision failure that makes a static number meaningless. A non-literal first argument is
         NOT admitted: the method would then be unknown too, and a row whose method and address are both
         unknown says only "a call happened", which no comparison can use. That exclusion is a FLOOR and is
         reported as `xhrOpenSkippedNonLiteralMethod`. */
      if (door && door.arg0Method) {
        const m = args[0];
        if (!m || m.type !== "StringLiteral" || !/^(GET|POST|PUT|DELETE|PATCH|HEAD|OPTIONS|TRACE)$/i.test(m.value)) {
          door = null; xhrOpenSkippedNonLiteralMethod++;
        }
      }
    }

    if (door && args) {
      const a = args[door.urlArg];
      const m0 = mutFloor.demoted;
      const r = a ? fold(a, binds, 0, envDoor) : { text: "{?}", holes: 1 };
      if (mutFloor.demoted > m0)
        mutFloor.rows.push({ chan: door.id, file: filename, line: n.loc ? n.loc.start.line : 0, url: r.text });
      const sig = r.holes ? holeSig(r) : null;
      const literalChars = r.text.replace(/\{\?\}/g, "").length;
      let kind;
      if (a && a.type === "StringLiteral") kind = "literal";
      else if (r.holes === 0) kind = "folded";
      else if (literalChars > 0) kind = "shape";
      else kind = "opaque";
      if (a) attached.add(a);
      /* WHETHER A STRONGER PARSER COULD HAVE DONE BETTER IS MEASURED HERE AND NEVER CONCEDED OR ASSUMED.
         This file's fold is deliberately conservative, so `opaque` is a FLOOR for the parse and the obvious
         objection is that a real commercial extractor with interprocedural constant propagation would fold
         more. That objection is answerable with two numbers rather than an opinion, and both are carried on
         every row: the SHAPE of the argument, and — where it is a bare name — HOW MANY TIMES THAT NAME IS
         BOUND IN ITS OWN FILE. A name bound once is resolvable by any parser and this file already folds it;
         a name bound a hundred times is a minifier's reused register, and no name-based resolution can touch
         it without a full scope graph AND the caller graph behind it.
         A CEILING PROBE THAT IGNORED SHADOWING WAS BUILT FIRST AND IS RECORDED AS REFUTED, because the
         wrong method is what a later reader would otherwise repeat: it answered "115 of 132 resolvable" and
         its own samples bound the URL name to `"custom"`, `"$default"`, `"replace"` and `"="`. It had
         measured NAME COLLISION IN MINIFIED CODE and not resolvability at all. The binding COUNT is the
         sound form of the same question and needs no judgement to read. */
      const argShape = !a ? "absent"
        : a.type === "Identifier" ? "Identifier"
        : (a.type === "MemberExpression" && a.object && a.object.type === "ThisExpression") ? "this.member"
        : a.type;
      const argBinds = (a && a.type === "Identifier") ? (bindCount.get(a.name) || 0) : null;
      sites.push({
        argShape, argBinds, argNameLen: (a && a.type === "Identifier") ? a.name.length : null,
        door: door.id, cls: door.cls, engineDoor: door.engine !== null, kind, holes: r.holes,
        url: r.text, alt: r.alt || null, guard,
        /* THE REACH PAIR. `fnDepth` 0 is a call the module body itself performs, so evaluating the program
           reaches it; anything above 0 needs its enclosing function CALLED. `innerAsync` is whether the
           INNERMOST enclosing function is an async one, which is the axis the two door classes come apart
           on: a call inside an `async` body runs only once something invokes that body, and in a real SPA
           the invoker is an effect flushed after a render commit, an event handler, a timer or an idle
           callback. Both are properties of the TEXT and neither is a claim about any engine. */
        fnDepth: fnStack.length,
        innerAsync: fnStack.length > 0 ? !!fnStack[fnStack.length - 1].async : false,
        method: door.arg0Method ? args[0].value.toUpperCase() : (door.id === "sendBeacon" ? "POST" : "GET"),
        line: n.loc ? n.loc.start.line : 0, file: filename, sig,
      });
    }
  }, (n) => {
    if (GUARDS.has(n.type) || n.type === "LogicalExpression") { guardStack.pop(); guard--; }
    if (FN_SCOPES.has(n.type)) fnStack.pop();
  });


  /* ── HOW THIS PROGRAM SPELLS THE DECLARED ENTRY NAMES ───────────────────────────────────────────────────
     A SEPARATE WALK, DELIBERATELY, AND THE REASON IS CALIBRATION RATHER THAN CLARITY. Folding this into the
     door walk above would have put a new branch inside the classifier every existing number is produced by,
     and the whole worth of a figure added to this file is that the figures already here did not move. A
     third traversal costs a fraction of the parse it rides on and buys an A/B nobody has to argue about.
     WHAT IS NOT A REFERENCE IS MARKED BY ITS PARENT ON THE WAY DOWN, which needs no parent chain: this
     walker enters a node strictly before its children, so a property name, an object key, a declaration id,
     a parameter and an import local are all struck from the reference population by the node that owns them
     before the identifier itself is reached. Without that, an object literal carrying a `fetch` key would
     read as a program naming the platform's fetch, which is the precision failure that makes the bare count
     useless in the direction that hides the floor.
     AN ALIAS IS NOT A SPELLING OF ITS OWN AND IS DELIBERATELY NOT A COLUMN. `const f = fetch` reaches the
     name by the bare spelling and `const f = window.fetch` by the property spelling, so both are already
     attributed where they happen; a further column would double-count one occurrence under two headings.
     DESTRUCTURING IS THE ONE EXCEPTION, because there the name appears only as a PATTERN KEY and in no
     reference position at all, so nothing else would see it. */
  const spell = new Map();                                  // declared name -> its spelling tally
  const spellOther = { globalComputedDynamic: 0 };
  const spellHit = (nm, k) => {
    let t = spell.get(nm);
    if (!t) { t = spellTally(); spell.set(nm, t); }
    t[k]++;
  };
  const notRef = new Set();
  /* THIS BAND KEEPS ITS OWN BINDER SET AND DOES NOT REUSE `collectBinds`, AND THE REASON IS A DIRECTION RATHER
     THAN A PREFERENCE. `collectBinds` walks EVERY identifier under a pattern and binds it, which is right for
     its own question — may this name be folded — because over-binding there REFUSES a fold. Asked this
     question it over-binds in the direction that hides the floor: `function f(a = fetch())` binds `a` and
     REFERENCES fetch, and counting that reference as a bound name moves it out of the column the landed row
     already sees and into the column this pass cannot decide, which reports the property spelling as more
     necessary than it is. THE SELF-TEST ROW FOR THAT SHAPE IS WHAT CAUGHT IT and is why it is a control.
     A BINDING POSITION FEEDS BOTH SETS AND A PROPERTY NAME FEEDS ONLY ONE, which is the whole of why they are
     two sets: an object key and a member's property name are struck from the reference population and bind
     nothing, so folding them into a binder set would make every file that carries a `fetch` KEY read as a
     file that shadows fetch. */
  const spellBound = new Set();
  const bindName = (id) => { if (id && id.type === "Identifier") { notRef.add(id); spellBound.add(id.name); } };
  const markPattern = (p) => {
    if (!p) return;
    switch (p.type) {
      case "Identifier": bindName(p); return;
      case "AssignmentPattern": markPattern(p.left); return;
      case "RestElement": markPattern(p.argument); return;
      case "ArrayPattern": for (const e of p.elements || []) markPattern(e); return;
      case "ObjectPattern":
        for (const q of p.properties || []) {
          if (q.type === "ObjectProperty") { if (!q.computed && q.key) notRef.add(q.key); markPattern(q.value); }
          else if (q.type === "RestElement") markPattern(q.argument);
        }
        return;
      default: return;
    }
  };
  walk(ast, (n) => {
    switch (n.type) {
      case "MemberExpression":
      case "OptionalMemberExpression": {
        if (!n.computed && n.property && n.property.type === "Identifier") {
          notRef.add(n.property);
          const nm = n.property.name;
          if (ENTRY_NAMES.has(nm)) {
            if (provenGlobal(n.object)) spellHit(nm, "qualified");
            else if (n.object && n.object.type === "Identifier" && GLOBAL_OBJECTS.has(n.object.name))
              spellHit(nm, "qualifiedBoundGlobal");
            else spellHit(nm, "instanceMember");
          }
        } else if (n.computed && n.property) {
          const k = n.property;
          if (k.type === "StringLiteral" && ENTRY_NAMES.has(k.value))
            spellHit(k.value, provenGlobal(n.object) ? "computedLiteral" : "instanceMemberComputed");
          /* A COMPUTED KEY THIS PASS CANNOT READ BELONGS TO NO NAME AND IS COUNTED APART. `self[n]` may be
             any member of the global object, so attributing it to one would invent a population; leaving it
             out entirely would let a corpus that reaches everything dynamically read as reaching nothing. */
          else if (k.type !== "StringLiteral" && provenGlobal(n.object)) spellOther.globalComputedDynamic++;
        }
        break;
      }
      case "ObjectProperty":
      case "ObjectMethod":
      case "ClassMethod":
      case "ClassPrivateMethod":
      case "ClassProperty":
        if (!n.computed && n.key) notRef.add(n.key);
        break;
      case "VariableDeclarator": {
        if (n.id && n.id.type === "Identifier") bindName(n.id);
        else markPattern(n.id);
        /* THE ONE SHAPE NO REFERENCE POSITION WOULD SHOW. */
        if (n.id && n.id.type === "ObjectPattern" && provenGlobal(n.init)) {
          for (const p of n.id.properties || []) {
            if (p.type !== "ObjectProperty" || p.computed) continue;
            const k = p.key;
            const nm = k && (k.type === "Identifier" ? k.name : k.type === "StringLiteral" ? k.value : null);
            if (nm && ENTRY_NAMES.has(nm)) spellHit(nm, "destructuredFromGlobal");
          }
        }
        break;
      }
      case "FunctionDeclaration":
      case "FunctionExpression":
      case "ClassDeclaration":
      case "ClassExpression":
      case "ArrowFunctionExpression":
        if (n.id) bindName(n.id);
        for (const p of n.params || []) markPattern(p);
        break;
      case "CatchClause":
        markPattern(n.param);
        break;
      /* AN ASSIGNMENT IS NOT A BINDING AND IS STILL DISQUALIFYING, for `provenGlobal`'s own reason: a name the
         file WRITES may mean something other than the platform's by the time it is read. */
      case "AssignmentExpression":
        if (n.left && n.left.type === "Identifier") spellBound.add(n.left.name);
        break;
      case "UpdateExpression":
        if (n.argument && n.argument.type === "Identifier") spellBound.add(n.argument.name);
        break;
      case "ImportSpecifier":
      case "ImportDefaultSpecifier":
      case "ImportNamespaceSpecifier":
        if (n.local) bindName(n.local);
        if (n.imported) notRef.add(n.imported);
        break;
      case "ExportSpecifier":
        if (n.local) notRef.add(n.local);
        if (n.exported) notRef.add(n.exported);
        break;
      case "LabeledStatement":
      case "BreakStatement":
      case "ContinueStatement":
        if (n.label) notRef.add(n.label);
        break;
      case "UnaryExpression":
        /* WHICH OF THE TWO READS THE LANDED ROW WOULD HAVE RECORDED. The non-throwing form the unary parser
           patches in for `typeof` is the one a feature test uses, so a name reached only that way is a
           program PROBING for a capability rather than using it — a distinction the engine's own row carries
           in its second argument and this column exists to be read against. */
        if (n.operator === "typeof" && n.argument && n.argument.type === "Identifier" &&
            ENTRY_NAMES.has(n.argument.name)) spellHit(n.argument.name, "typeofBare");
        break;
      default: break;
    }
  });
  /* A SECOND PASS FOR THE BARE COLUMN, BECAUSE A REFERENCE MAY PRECEDE ITS OWN BINDING. A hoisted declaration
     and a function body that runs before the `var` below it both put the reference first in source order, so
     classifying a bare name during the marking pass would read the same program two ways depending on where
     its binding happened to be written. */
  walk(ast, (n) => {
    if (n.type !== "Identifier" || notRef.has(n) || !ENTRY_NAMES.has(n.name)) return;
    spellHit(n.name, spellBound.has(n.name) ? "bareBoundName" : "bareFree");
  });

  /* THE BASE RATE. Every string literal in this program that looks like an address and is NOT the URL
     argument of a door — what a naive extractor would report and what this one deliberately does not. */
  walk(ast, (n) => {
    /* THE DECLARED BLIND SPOT, COUNTED RATHER THAN DESCRIBED. `el.src = url` / `el.href = url` is a door
       `html_script.c` and `html_link.c` DO record and the site channel above deliberately does not, because
       `.src` is a property of many things that are not elements. That exclusion is right and it was stated
       as a SENTENCE carrying two numbers frozen at a past corpus, which is the shape this file's own header
       forbids: a floor that names what it excludes without measuring it is read as a total. It is measured
       here, on every run, per site, in the same four kinds as a door — so a PROGRAM-door zero can be read
       against it. These rows are NOT sites and are summed into no door total; they are the size of what the
       door set cannot see, printed where a zero would otherwise be read as a clean bill. */
    if (n.type === "AssignmentExpression" && n.operator === "=") {
      const L = n.left;
      if (!L || L.type !== "MemberExpression" || L.computed || L.property.type !== "Identifier") return;
      if (L.property.name !== "src" && L.property.name !== "href") return;
      /* THE SAME ENV AS THE DOORS, because the blind spot is the size a door ZERO is read against and a
         blind spot measured by a WEAKER fold than the doors it explains would overstate itself — which is
         the one direction a blind spot must not be wrong in, since a blind spot stated too large certifies
         nothing while one stated too small is read as a clean bill. Its opaque share moving DOWN is the
         honest consequence of the doors' own share moving down. */
      const m0 = mutFloor.demoted;
      const r = fold(n.right, binds, 0, envDoor);
      if (mutFloor.demoted > m0)
        mutFloor.rows.push({ chan: "." + L.property.name, file: filename, line: n.loc ? n.loc.start.line : 0, url: r.text });
      const literalChars = r.text.replace(/\{\?\}/g, "").length;
      const kind = n.right.type === "StringLiteral" ? "literal"
        : r.holes === 0 ? "folded" : literalChars > 0 ? "shape" : "opaque";
      blind.push({ prop: L.property.name, kind, url: r.text, file: filename,
                   line: n.loc ? n.loc.start.line : 0, sig: r.holes ? holeSig(r) : null });
      return;
    }
    if (n.type !== "StringLiteral" || attached.has(n)) return;
    const v = n.value;
    if (v.length < 2 || v.length > 512) return;
    /* THE BASE RATE KEEPS ITS OWN TWO ALTERNATIVES. `looksLikeAddress` adds a third for the manifest, and a
       relative `./x` counted here would move a number other lanes price against, so this asks for the two
       it always asked for. The shared helper is what stops the two channels disagreeing about the two. */
    if (/^https?:\/\/[^\s]+$/.test(v) || /^\/[A-Za-z0-9_][^\s"'<>]*$/.test(v)) pathish.add(v);
  });

  /* ── THE `a call result` SECOND FIELD, RUN ONCE PER FILE AFTER EVERY FOLD IS DONE ────────────────────────
     THE CENSUS ENV CARRIES NO `callHoles`, AND THAT IS LOAD-BEARING RATHER THAN TIDY: the folds below fold a
     callee's RETURN, which can hole at a call of its own, and a recording env would add to the very Set this
     loop is iterating. It also carries its own throwaway budgets so the closure, inline and mutable-fold
     PRICES this file prints stay figures about the door channel and not about this census. */
  const callResult = { verdict: new Map(), rows: [] };
  {
    const cenv = { ...envBase, vars: new Map(), app: null, fn: null, candidate: null,
                   inline: { seen: new Set(), settled: 0, refused: new Map() },
                   sole: { seen: new Set(), settled: 0, unsettled: 0, refused: new Map() },
                   mutFloor: { demoted: 0, rows: [] } };
    for (const call of envDoor.callHoles) {
      const v = calleeVerdict(call, binds, cenv);
      callResult.verdict.set(v.verdict, (callResult.verdict.get(v.verdict) || 0) + 1);
      if (v.verdict === CALLEE_IN_FILE || v.verdict === CALLEE_NOT_TEXT)
        callResult.rows.push({ v: v.verdict, param: !!v.param, file: filename,
                               line: call.loc ? call.loc.start.line : 0,
                               callee: spellCallee(call.callee), text: v.text });
    }
  }

  return { parsed: true, error: null, sites, pathish, blind, xhrOpenSkippedNonLiteralMethod, globalDoor,
           manifest, spell, spellOther,
           inline: { settled: inlineBudget.settled, refused: inlineBudget.refused }, recvDoor,
           sole: { settled: soleBudget.settled, unsettled: soleBudget.unsettled, refused: soleBudget.refused },
           mutFloor, callResult };
}

/* ── THE ARMED CONTROL ────────────────────────────────────────────────────────────────────────────────────
   RUN ON EVERY INVOCATION, BEFORE ANY CORPUS IS READ, AND FATAL. CLAUDE.md §THE-ORDER-IS-FIXED-AND-IT-IS-TWO-
   RUNS: a probe whose expected output is silence, with no run in which the same probe shape SPOKE, has
   calibrated nothing — so each row below is an input this file must classify a stated way, and the NEGATIVE
   rows are inputs it must NOT see at all. A classifier that silently stopped matching `fetch` would report a
   smaller surface, and a smaller surface is the flattering direction here: it would read as "the parse
   recovers less", which is the answer that argues FOR the engine. This control is what stops that being
   indistinguishable from a true finding.
   EVERY `kind` AND EVERY `cls` APPEARS AT LEAST ONCE BELOW, asserted after the table runs, so a kind that
   became unreachable cannot go quiet — which is the defect a table of examples nobody counts always has. */
const SELFTEST = [
  // [ source, expected rows as `door|cls|kind|url` ... ]
  [`fetch("/api/users")`,                                   ["fetch|data|literal|/api/users"]],
  [`const B="/api/v2";fetch(B+"/users")`,                   ["fetch|data|folded|/api/v2/users"]],
  [`fetch("/api/"+region)`,                                 ["fetch|data|shape|/api/{?}"]],
  ["fetch(`/api/${r}/x`)",                                  ["fetch|data|shape|/api/{?}/x"]],
  [`fetch(u)`,                                              ["fetch|data|opaque|{?}"]],
  [`fetch(u.v)`,                                            ["fetch|data|opaque|{?}"]],
  /* THE WIDENINGS THIS DIFF LANDED, ARMED — every one of them, because a fold that is never exercised is a
     fold whose count is unarmed, and the URL arms are the ones whose WRONG answer would be an INVENTED
     address rather than a missing one. The exact, the base-ignored, the suffix and the MUTABLE readings are
     four different answers to `new URL` and each is stated, so a change that collapses two of them cannot
     pass quietly. */
  [`fetch(new URL("https://h.example/a/b"))`,               ["fetch|data|folded|https://h.example/a/b"]],
  [`fetch(new URL("/a/b","https://h.example/x/y"))`,        ["fetch|data|folded|https://h.example/a/b"]],
  [`fetch(new URL("https://o.example/z","https://h.example/x"))`, ["fetch|data|folded|https://o.example/z"]],
  [`fetch(new URL("./w.js",import.meta.url))`,              ["fetch|data|shape|{?}/w.js"]],
  [`fetch(new URL("../a/b.js",q))`,                         ["fetch|data|shape|{?}/a/b.js"]],
  [`fetch(new URL("?x=1",q))`,                              ["fetch|data|opaque|{?}"]],
  [`const u=new URL("https://h.example/a");fetch(u.toString())`, ["fetch|data|shape|https://h.example/a{?}"]],
  [`const u=new URL("https://h.example/a");fetch(u)`,       ["fetch|data|shape|https://h.example/a{?}"]],
  [`fetch(String("/a/b"))`,                                 ["fetch|data|folded|/a/b"]],
  [`fetch(new Request("/a/b"))`,                            ["fetch|data|folded|/a/b"]],
  [`fetch("/a/".concat("b"))`,                              ["fetch|data|opaque|{?}"]],  /* .concat is refused: Array.prototype.concat is the same shape */
  [`fetch(encodeURIComponent("/a/b"))`,                     ["fetch|data|opaque|{?}"]],
  [`const f=e=>"/api/"+e+"/x";fetch(f("v2"))`,              ["fetch|data|folded|/api/v2/x"]],
  [`const g=()=>"/api/g";fetch(g())`,                       ["fetch|data|folded|/api/g"]],
  [`const h=e=>"/api/"+e;fetch(h(q))`,                      ["fetch|data|opaque|{?}"]],
  [`const i=async e=>"/api/"+e;fetch(i("v"))`,              ["fetch|data|opaque|{?}"]],
  [`const j=e=>(e=>"/api/"+e)(1);fetch(j("v"))`,            ["fetch|data|opaque|{?}"]],
  [`const k=(a,b)=>"/api/"+a;fetch(k("v"))`,                ["fetch|data|opaque|{?}"]],
  [`var o={};o.p="/pub/";o.u=e=>e+".js";fetch(o.p+o.u("c"))`, ["fetch|data|folded|/pub/c.js"]],
  [`navigator.serviceWorker.register("/sw.js")`,            ["serviceWorker.register|program|literal|/sw.js"]],
  [`container.register("/sw.js")`,                          []],
  [`const R={u:"/a/b"};fetch(R.u)`,                          ["fetch|data|folded|/a/b"]],
  [`const P="/p";fetch(c?P:"/q")`,                          ["fetch|data|folded|/p"]],
  [`x.open("GET","/t")`,                                    ["xhr.open|data|literal|/t"]],
  [`x.open("POST",u)`,                                      ["xhr.open|data|opaque|{?}"]],
  [`navigator.sendBeacon("/b",d)`,                          ["sendBeacon|data|literal|/b"]],
  [`import("./c.js")`,                                      ["import()|program|literal|./c.js"]],
  [`new Worker("/w.js")`,                                   ["new Worker|program|literal|/w.js"]],
  [`new WebSocket("wss://h/s")`,                            ["new WebSocket|data|literal|wss://h/s"]],
  [`if(a){fetch("/g")}`,                                    ["fetch|data|literal|/g"]],
  // THE GLOBAL OBJECT REACHING A PLATFORM DOOR. `fetch(u)` and `window.fetch(u)` are one platform name and
  // must classify identically; a row here that stopped matching would shrink the baseline, which is the
  // flattering direction and the one this control exists to make impossible to mistake for a finding.
  [`window.fetch("/api/a")`,                                ["fetch|data|literal|/api/a"]],
  [`self.fetch(u)`,                                         ["fetch|data|opaque|{?}"]],
  [`new self.Worker("/w.js")`,                              ["new Worker|program|literal|/w.js"]],
  [`new globalThis.WebSocket("wss://h/s")`,                 ["new WebSocket|data|literal|wss://h/s"]],
  [`globalThis.fetch("/api/"+r)`,                           ["fetch|data|shape|/api/{?}"]],
  // NEGATIVES — a classifier that reports any of these is over-counting, which is the direction that would
  // make the parse look stronger than it is and the engine's contribution smaller than it is.
  [`window.open("/x","_blank")`,                            []],
  [`db.open("GET")`,                                        []],
  [`m.get("/api/x")`,                                       []],
  [`const s="/api/looks-like-an-endpoint"`,                 []],
  [`x.open(method,"/t")`,                                   []],
  // A RECEIVER THAT CARRIES INFORMATION IS STILL REFUSED, which is the precision half of the widening above
  // and is the larger population by an order of magnitude: admitting any of these would put library-wrapper
  // method calls on the static side of a comparison the engine never answered for.
  [`api.fetch("/x")`,                                       []],
  [`this.fetch("/x")`,                                      []],
  [`mk().fetch("/x")`,                                      []],
  [`new p.Worker("/w.js")`,                                 []],
  // A GLOBAL NAME THIS FILE BINDS IS NOT THE GLOBAL OBJECT — the UMD wrapper, and every `var self=this`.
  [`function f(window){return window.fetch("/x")}`,         []],
  [`var self=this;self.fetch("/x")`,                        []],
  // SHADOWING — A REFERENCE IS RESOLVED, NEVER REFUSED FOR A BINDING SOMEWHERE ELSE. THIS ROW USED TO WANT
  // `opaque|{?}` and the argument for that is kept because a reader will re-derive it: the fold must refuse
  // a name the file binds twice, because folding ACROSS a shadow would INVENT an address. The premise is the
  // absence of a scope graph and not any property of a name — there are two bindings here and this reference
  // names exactly one of them, so the inner `const B="/b"` is the answer and `/b` is what a browser reads.
  // These four are the widening's own armed control and the reason its direction can be checked rather than
  // argued: the first two are the same file read from inside and from outside the shadow, and they must give
  // DIFFERENT answers or the pass is keying on a name after all.
  [`const B="/a";function f(){const B="/b";return fetch(B)}`, ["fetch|data|folded|/b"]],
  [`const B="/a";fetch(B);function f(){const B="/b"}`,      ["fetch|data|folded|/a"]],
  //  an inner shadow that is WRITTEN poisons ITS OWN binding and not the outer one
  [`var B="/a";fetch(B);function f(){var B;B="/z"}`,        ["fetch|data|folded|/a"]],
  //  ... and a write to the binding the reference actually resolves to still refuses it
  [`var B="/a";fetch(B);B="/z"`,                            ["fetch|data|opaque|{?}"]],
  //  TWO DECLARATIONS IN ONE SCOPE ARE STILL TWO, so the name settles nothing and the fold declines
  [`var B="/a";var B="/b";fetch(B)`,                        ["fetch|data|opaque|{?}"]],
  //  A `with` BODY IS REFUSED OUTRIGHT: a bare name in one may denote a PROPERTY of the object, and no
  //  lexical resolution answers for it. Without this the pass would fold `B` and a browser might not.
  [`var B="/a";with(o){fetch(B)}`,                          ["fetch|data|opaque|{?}"]],
  // THE REACH BAND, ARMED IN ALL THREE ARMS. Each of these has to CLASSIFY as an ordinary row too, so they
  // sit in this table rather than in a set of their own: a reach control that stopped being a door row would
  // silently leave the band measuring a smaller population.
  [`async function f(){return fetch("/ra")}`,               ["fetch|data|literal|/ra"]],
  [`function f(){return fetch("/rs")}`,                     ["fetch|data|literal|/rs"]],
  [`const g=async()=>fetch("/rq")`,                         ["fetch|data|literal|/rq"]],
  [`async function o(){return function(){return fetch("/rn")}}`, ["fetch|data|literal|/rn"]],
  /* ── THE UNIQUE-CALL-SITE CLOSURE, EVERY ARM AND EVERY REFUSAL ─────────────────────────────────────────
     THE POSITIVE ROWS FIRST, because a closure whose refusals are armed and whose acceptance is not is a
     mechanism nobody has shown working. The negatives below are the ones that matter more: every one of them
     is a shape where binding the parameter to one call's argument would state an address some OTHER entry
     into the function never passes, which is this file INVENTING one — the single failure it may not have. */
  [`function f(u){fetch(u)}f("/a/b")`,                      ["fetch|data|folded|/a/b"]],
  [`const f=u=>fetch(u);f("/a/"+r)`,                        ["fetch|data|shape|/a/{?}"]],
  [`(u=>fetch(u))("/iife")`,                                ["fetch|data|folded|/iife"]],
  [`function f(a,b){fetch(b)}f("/x","/y")`,                 ["fetch|data|folded|/y"]],
  //  THE SCOPE CONTROL, AND IT IS THE ONE THAT SAYS THIS IS NOT A NAME MATCH: two functions whose parameter
  //  is spelled the same, one nested in the other, and the door inside the inner one. `/in` is the answer a
  //  browser reads; `/out` is what a pass keying on the NAME would report.
  [`function f(u){function g(u){fetch(u)}g("/in")}f("/out")`, ["fetch|data|folded|/in"]],
  //  A SECOND CALL SITE MEANS THE PARAMETER IS NOT DETERMINED — the first refusal, and the commonest.
  [`function f(u){fetch(u)}f("/a");f("/b")`,                ["fetch|data|opaque|{?}"]],
  //  A RECURSIVE FUNCTION REFERENCES ITS OWN NAME, so it can never qualify and needs no separate guard.
  [`function f(u){fetch(u);f(u)}f("/a")`,                   ["fetch|data|opaque|{?}"]],
  //  THE REFERENCE IS NOT A CALLEE — passed as a value, reached through `.call`, or constructed. Each of
  //  these can be entered with arguments this file never sees.
  [`function f(u){fetch(u)}g(f)`,                           ["fetch|data|opaque|{?}"]],
  [`function f(u){fetch(u)}f.call(null,"/a")`,              ["fetch|data|opaque|{?}"]],
  [`function f(u){fetch(u)}new f("/a")`,                    ["fetch|data|opaque|{?}"]],
  //  THE FUNCTION IS NOT A NAMED VALUE AT ALL — an object method, which is the bundler's own module shape
  //  and the dominant refusal over the corpus. There is no reference for any count to be about.
  [`const o={f(u){fetch(u)}};o.f("/a")`,                    ["fetch|data|opaque|{?}"]],
  //  ... AND THE OTHER TWO POPULATIONS THAT REFUSAL COVERS, armed separately because they take OPPOSITE
  //  work. A function that is the VALUE OF A MEMBER WRITE is reachable by the once-written question
  //  `slotOf` already answers one level over, so its count is a queue; a CLASS METHOD needs the receiver's
  //  type, which is a recogniser; and a CALLBACK ARGUMENT has its parameter bound by the CALLEE — a
  //  resolution value, an array element, an Event — so no argument exists in this file to read at all and
  //  its count is not a queue for any parse. One number over the three would say none of that.
  [`var o={};o.f=u=>fetch(u);o.f("/a")`,                    ["fetch|data|opaque|{?}"]],
  [`class C{m(u){fetch(u)}}`,                              ["fetch|data|opaque|{?}"]],
  [`p.then(u=>fetch(u))`,                                  ["fetch|data|opaque|{?}"]],
  //  A SPREAD MAKES POSITION MEANINGLESS, and a call short of the parameter supplies nothing to fold.
  [`function f(u){fetch(u)}f(...a)`,                        ["fetch|data|opaque|{?}"]],
  [`function f(u,v){fetch(v)}f("/a")`,                      ["fetch|data|opaque|{?}"]],
  //  THE PARAMETER IS REWRITTEN INSIDE THE BODY, so what the call passed is not what the door reads. This is
  //  the row whose WRONG answer would be an address the program never requests.
  [`function f(u){u="/z";fetch(u)}f("/a")`,                 ["fetch|data|opaque|{?}"]],
  //  `arguments` ALIASES A PARAMETER IN SLOPPY MODE and leaves no write any of these passes can see, so a
  //  function mentioning the name is refused outright rather than reasoned about.
  [`function f(u){arguments[0]="/z";fetch(u)}f("/a")`,      ["fetch|data|opaque|{?}"]],
  //  A PATTERN, A DEFAULT AND A REST each stand for something other than "argument i".
  [`function f({u}){fetch(u)}f({u:"/a"})`,                  ["fetch|data|opaque|{?}"]],
  [`function f(u="/d"){fetch(u)}f("/a")`,                   ["fetch|data|opaque|{?}"]],
  //  A FUNCTION NOBODY CALLS HAS NO ARGUMENT TO READ, and one whose BINDING is rewritten could be any
  //  function at all by the time the call runs — two refusals that look like the others and are not,
  //  because neither is about how many times the name is referenced.
  [`function f(u){fetch(u)}`,                               ["fetch|data|opaque|{?}"]],
  [`var f=u=>fetch(u);f=null`,                              ["fetch|data|opaque|{?}"]],
  //  TWO HELPERS CALLING ONLY EACH OTHER are unreachable code and still a CYCLE in this graph, so the walk
  //  carries its own seen set rather than trusting the depth limit to end it.
  [`function f(u){g(u)}function g(v){f(v);fetch(v)}`,       ["fetch|data|opaque|{?}"]],
  /* ── THE WHY BAND, EVERY CLASS ARMED SEPARATELY ─────────────────────────────────────────────────────────
     ONE CONTROL PER CLASS AND NOT ONE PER BAND, for the reason this file's own diff is the evidence for:
     arming one of four populations left three rows as counts nobody had watched fire, and a class that has
     never spoken cannot be told from a class that is dead. These are the door rows' SIGNATURES, so each of
     these sources must ALSO classify as an ordinary row in the table above — a control that stopped being a
     door row would quietly leave the band measuring a smaller population. */
  [`a.map(u=>fetch(u))`,                                    ["fetch|data|opaque|{?}"]],
  [`el.addEventListener("x",u=>fetch(u))`,                  ["fetch|data|opaque|{?}"]],
  [`requestAnimationFrame(u=>fetch(u))`,                    ["fetch|data|opaque|{?}"]],
  [`hof(u=>fetch(u))`,                                      ["fetch|data|opaque|{?}"]],
  [`t[k](u=>fetch(u))`,                                     ["fetch|data|opaque|{?}"]],
  [`fetch(u[k])`,                                           ["fetch|data|opaque|{?}"]],
  [`var k="b";var m={a:"/x"};fetch(m[k])`,                  ["fetch|data|opaque|{?}"]],
  [`fetch(a-b)`,                                            ["fetch|data|opaque|{?}"]],
  [`fetch([1])`,                                            ["fetch|data|opaque|{?}"]],
];
/* THE SIGNATURE EACH CONTROL MUST PRODUCE. Kept in its own table rather than folded into the row string
   above, because a signature is a claim about WHY a row is incomplete and the row string is a claim about
   WHAT it recovered — one table asserting both would make a change to either look like a change to the
   other. The assertion over these is TWO-SIDED: every class this file declares must be produced by some
   control, AND every class any control produces must be declared, so the declared list cannot drift away
   from what the classifier can actually emit. */
const SELFTEST_WHY = new Map([
  [`p.then(u=>fetch(u))`,                   `a promise resolution value`],
  [`a.map(u=>fetch(u))`,                    `an array element`],
  [`el.addEventListener("x",u=>fetch(u))`,  `an Event`],
  [`requestAnimationFrame(u=>fetch(u))`,    `a platform timestamp`],
  [`hof(u=>fetch(u))`,                      `a callback parameter supplied by app code`],
  [`t[k](u=>fetch(u))`,                     `a callback parameter whose supplier this pass cannot name`],
  [`function f(u){fetch(u)}f("/a");f("/b")`, `a function parameter, no platform supplier`],
  [`fetch(u)`,                              `a global this file never assigns`],
  [`var B="/a";var B="/b";fetch(B)`,        `a name this pass refuses to settle`],
  [`fetch(u.v)`,                            `a property read`],
  [`fetch(u[k])`,                           `a computed property read`],
  [`var k="b";var m={a:"/x"};fetch(m[k])`,  `a map key the object does not carry`],
  [`fetch(encodeURIComponent("/a/b"))`,     `a call result`],
  [`fetch(a-b)`,                            `an operator this fold does not evaluate`],
  [`fetch([1])`,                            `a node kind this fold has no arm for`],
  [`const u=new URL("https://h.example/a");fetch(u)`, `a mutation nothing in the text records`],
]);
/* THE SECOND FIELD'S OWN CONTROLS, ONE PER VERDICT AND ASSERTED TWO-SIDED. CLAUDE.md
   §A-CONTROL-ARMS-ONLY-ON-A-SITE-THE-INSTRUMENT-CAN-JUDGE: a verdict nobody has watched fire cannot be told
   apart from a verdict that is dead, and this census's rows are read as a partition of the band's largest
   class — so a dead verdict would silently move that mass into whichever verdict still spoke. The forward
   direction says every verdict this file DECLARES has been produced; the reverse says every verdict produced
   has been declared, which is the only check that sees a verdict added to `calleeVerdict` and not to the list.
   EACH CONTROL IS ASSERTED BY MEMBERSHIP RATHER THAN BY EQUALITY, because one door argument can carry several
   call holes and pinning the whole set would make the control a claim about how many rather than about which. */
const SELFTEST_CALLEE = new Map([
  /* THESE TWO ROWS READ `function g(){return "/a/b"}fetch(g())` AND `function g(){return u}fetch(g())` AND
     BOTH PRODUCED AN EMPTY VERDICT SET, which is recorded here rather than quietly swapped because the reason
     is the whole of what this census is FOR. A one-statement body with matching arity is a shape the door
     channel's INLINE ARM ALREADY CROSSES, so the fold enters the callee and the hole is attributed INSIDE it
     at its own origin — `fetch(g())` on `return u` reports `a global this file never assigns` and never
     `a call result`. The band's `a call result` class is therefore EXACTLY the population that arm REFUSED,
     and a control drawn from the population it accepts arms nothing. Each row below is a shape the inline
     refuses by its own stated reason (a body of more than one statement; an argument that does not settle). */
  [`function g(){var x=1;return "/a/b"}fetch(g())`,  `its return folds to text from this file alone`],
  [`function g(a){return u}fetch(g(u))`,             `its return does not fold from this file's text`],
  [`function g(){}fetch(g())`,                  `its definition returns nothing this pass can read`],
  [`var g="/a";fetch(g())`,                     `the callee resolves to a value that is not a function`],
  [`fetch(g())`,                                `the callee is a name this file never binds`],
  [`var g=function(){return "/a"};var g=function(){return "/b"};fetch(g())`,
                                                `the callee is a name this pass will not settle`],
  [`fetch(o.f())`,                              `the callee is a property this pass cannot settle`],
  [`fetch(o[k]())`,                             `the callee is a computed property read`],
  [`fetch(h()())`,                              `the callee is not a name at all`],
  [`fetch(new Q())`,                            `it constructs an object rather than returning a value`],
]);
/* THE PARAMETER BIT ARMED IN BOTH DIRECTIONS AND AT BOTH ENDS. A bit whose only control is a positive one
   cannot tell "no resolved callee in this corpus uses a parameter" from "the test is stuck on", and this bit
   decides which of two next diffs a reader is looking at — so both values are asserted to have been produced.
   The two sources differ in exactly one character of the return expression, which is what makes the pair a
   control on the BIT rather than on two unrelated shapes. */
const SELFTEST_CALLEE_PARAM = new Map([
  [`function g(a){return u+a}fetch(g(u))`, true],
  [`function g(a){return u}fetch(g(u))`,   false],
]);
const SELFTEST_GUARDED = new Set([`if(a){fetch("/g")}`]);
/* THE REACH BAND'S OWN CONTROLS — source to the pair the row must carry. It is armed in BOTH directions and
   at BOTH ends, which is the discipline CLAUDE.md §A-CONTROL-ARMS-ONLY-ON-A-SITE-THE-INSTRUMENT-CAN-JUDGE
   asks for: a band whose only control is a positive one cannot tell "this corpus has no async sites" from
   "the async test is stuck on". The fourth row is the one that matters most and is the easiest to get wrong
   — an async function enclosing a SYNC one — because `innerAsync` names the INNERMOST enclosing function and
   a walker that read the OUTERMOST would pass the first three and fail only here. */
const SELFTEST_REACH = new Map([
  [`fetch("/api/users")`,                                   { fnDepth: 0, innerAsync: false }],
  [`async function f(){return fetch("/ra")}`,                { fnDepth: 1, innerAsync: true }],
  [`function f(){return fetch("/rs")}`,                      { fnDepth: 1, innerAsync: false }],
  [`const g=async()=>fetch("/rq")`,                          { fnDepth: 1, innerAsync: true }],
  [`async function o(){return function(){return fetch("/rn")}}`, { fnDepth: 2, innerAsync: false }],
]);

/* THE BLIND-SPOT CHANNEL IS ARMED SEPARATELY AND IN BOTH DIRECTIONS. Its whole job is to be the number a
   PROGRAM-door zero is read against, so a channel that silently stopped counting would make every such zero
   read as a clean bill — the one reading CLAUDE.md §A-CONTROL-ARMS-ONLY-ON-A-SITE exists to forbid. Each
   positive row must be classified the stated way AND must produce NO site row, because a blind-spot row
   that leaked into `sites` would move a number this file's conclusions are drawn from. */
const SELFTEST_BLIND = [
  [`s.src="/a.js"`,                 ["src|literal|/a.js"]],
  [`const B="/b/";s.src=B+"c.js"`,  ["src|folded|/b/c.js"]],
  [`s.src="/x/"+e`,                 ["src|shape|/x/{?}"]],
  [`s.src=P+u(e)`,                  ["src|opaque|{?}{?}"]],
  [`l.href="/s.css"`,               ["href|literal|/s.css"]],
  // NEGATIVES — none of these is an `.src`/`.href` assignment and counting one would inflate the size of
  // the blind spot, which is the direction that would make the door set look worse than it is.
  [`s.srcset="/a.js"`,              []],
  [`s[k]="/a.js"`,                  []],
  [`s.src+="/a.js"`,                []],
  [`fetch("/api/x")`,               []],
];

/* THE CHUNK-MANIFEST CHANNEL IS ARMED ON BOTH SHAPES AND ON EVERY REFUSAL IT CLAIMS TO MAKE. Its number is
   an ADDRESS SET, so a row that silently stopped enumerating would report a smaller manifest — and a smaller
   manifest reads as "a parse cannot reach these after all", which is the flattering direction and exactly the
   answer this file must not manufacture. Each row states the addresses, how many candidates were drawn, and
   how many were DROPPED for a hole: the dropped figure is what arms the rule that a missing map key is not an
   empty string, and without it a candidate absent from the hash map would emit an address with a segment
   deleted from it. */
const SELFTEST_MANIFEST = [
  /* THE TERNARY-CHAIN FORM: a public path assigned to a slot, a chain assigned to another, composed. */
  [`var p={};p.u=e=>1===e?"a/1.js":2===e?"a/2.js":"a/x.js";p.p="/pub/";var b=p.p+p.u(e)`,
   { addresses: ["/pub/a/1.js", "/pub/a/2.js"], candidates: 2, dropped: 0 }],
  /* THE TWO-MAP FORM, WHICH IS THE ONE THE CORRELATION MATTERS FOR. `c` is in the name map and not the hash
     map, so it DROPS; `b` is in the hash map and not the name map, so `||` falls through to the key itself. */
  [`function t(e){return o.p+""+({a:"A",c:"C"}[e]||e)+"."+{a:"h",b:"g"}[e]+".chunk.js"}function o(){}o.p="/w/";s.src=t(e)`,
   { addresses: ["/w/A.h.chunk.js", "/w/b.g.chunk.js"], candidates: 3, dropped: 1 }],
  /* THE CORRELATION, ASSERTED AS AN EXACT LIST, WHICH IS THE ONLY CONTROL ON THE WHOLE DESIGN THAT MATTERS.
     Two maps are indexed by ONE parameter, so the answer is two addresses and not four: `na` pairs with `ha`
     and `nb` with `hb`, and the crossed pairs `./na.hb.js` and `./nb.ha.js` name nothing that exists. The
     `want` list is compared exactly, so a fold that enumerated the maps INDEPENDENTLY fails here — and it
     would fail loudly, because a cross-product grows as the square. Measured on one real runtime: 394
     addresses recovered where crossing the two maps would have emitted up to 155236. */
  [`function t(e){return "./"+{a:"na",b:"nb"}[e]+"."+{a:"ha",b:"hb"}[e]+".js"}var b=t(e)`,
   { addresses: ["./na.ha.js", "./nb.hb.js"], candidates: 2, dropped: 0, fragments: 0 }],
  /* A LABEL TABLE IS THE SAME SHAPE AS A CHUNK MANIFEST AND MUST RECOVER NO ADDRESS. This is the precision
     control and it is the one the channel was measured failing before the shape test existed. */
  [`function t(e){return {a:"session",b:"Users"}[e]}var b=t(e)`,
   { addresses: [], candidates: 2, dropped: 0, fragments: 2 }],
  /* A CHUNK NAME WITH NO PUBLIC PATH IS A FRAGMENT, not a shorter address. */
  [`function t(e){return {a:"app.HASH.css"}[e]}var b=t(e)`,
   { addresses: [], candidates: 1, dropped: 0, fragments: 1 }],
  /* A RELATIVE ADDRESS IS ONE, which is the third alternative the shape test adds for this channel. */
  [`function t(e){return "./chunks/"+{a:"a.HASH.js"}[e]}var b=t(e)`,
   { addresses: ["./chunks/a.HASH.js"], candidates: 1, dropped: 0, fragments: 0 }],
  /* THE SHAPE A FILE-WIDE NAME COUNT REFUSED AND A SCOPE PRE-PASS SETTLES, which is the whole of what this
     diff buys and is written from a real webpack-4 runtime rather than invented. The chunk-URL function `t`
     and the object `o` holding the public path are declared at the runtime's own scope; ONE NESTED FUNCTION
     binds `d,t,o` again as its own `var` and assigns `t` inside itself. That function encloses neither the
     declarations nor the composition, so every reference outside it resolves to the outer binding — and a
     count over the whole file saw `t` bound twice and written once and refused the site entirely. */
  [`function t(e){return o.p+({a:"A"}[e]||e)+".chunk.js"}function o(){}o.p="/w/";` +
   `function h(a){for(var d,t,o=a[0],s=0;s<o.length;s++)t=o[s],d=t;}s.src=t(e)`,
   { addresses: ["/w/A.chunk.js"], candidates: 1, dropped: 0 }],
  /* NEGATIVES — each one a shape whose fold would INVENT an address, and each refused for a stated reason. */
  //  two declarations of the chunk-URL function IN ONE SCOPE are two functions and neither is the answer
  [`function t(e){return "/a/"+{x:"1"}[e]}function t(e){return "/b/"+{x:"2"}[e]}var b=t(e)`, null],
  //  ... and a write to the binding the composition resolves to still refuses it
  [`function t(e){return "/a/"+{x:"1"}[e]}t=0;var b=t(e)`,                              null],
  //  a slot written twice is not a slot this pass can read
  [`var p={};p.u=e=>1===e?"/a":"/b";p.u=e=>"/z";var b=p.p+p.u(e)`,                     null],
  //  a computed write to the object could be this very slot under another spelling
  [`var p={};p.u=e=>1===e?"/a/1.js":"/b";p[k]=1;var b=p.u(e)`,                         null],
  //  a body with a statement before its return may narrow the parameter
  [`function t(e){var z=1;return "/x/"+{a:"A"}[e]}var b=t(e)`,                         null],
  //  an object bound more than once is not one object
  [`var p={};var p={};p.u=e=>1===e?"/a/1.js":"/b";var b=p.u(e)`,                       null],
  //  a function whose body names no candidate for its parameter enumerates nothing
  [`var p={};p.u=e=>"/static/"+e;p.p="/x/";var b=p.p+p.u(e)`,                          null],
];
/* A COMPOSITION HOLDING TWO APPLICATIONS IS REFUSED AND THE REFUSAL IS COUNTED, asserted apart because the
   expected row list is empty either way and an uncounted refusal is indistinguishable from a shape the
   scanner never saw. */
const SELFTEST_MANIFEST_TWO = `var p={};p.u=e=>1===e?"/a/1.js":"/b";p.v=e=>2===e?"/c/2.js":"/d";var b=p.u(e)+p.v(e)`;

function selftest() {
  const seenKind = new Set(), seenCls = new Set();
  let spoke = 0, reachSpoke = 0;
  const soleRefused = new Set();
  let soleSettled = 0, soleUnsettled = 0, mutDemoted = 0;
  const whyClasses = new Set(), whySeenPer = new Map();
  for (const [src, want] of SELFTEST) {
    const r = readFile(src, "<selftest>");
    if (!r.parsed) die(`SELF-TEST: the parser refused \`${src}\` — ${r.error}`);
    const got = r.sites.map((x) => `${x.door}|${x.cls}|${x.kind}|${x.url}`);
    if (got.join("\n") !== want.join("\n"))
      die(`SELF-TEST FAILED on \`${src}\`\n  want ${JSON.stringify(want)}\n  got  ${JSON.stringify(got)}\n` +
          `This file's classifier no longer does what its own numbers are read as meaning. Every figure ` +
          `below this point would be about a different question, so nothing is printed.`);
    for (const x of r.sites) { seenKind.add(x.kind); seenCls.add(x.cls); spoke++; }
    for (const k of r.sole.refused.keys()) soleRefused.add(k);
    for (const x of r.sites) if (x.sig) { for (const c of x.sig.split(" + ")) whyClasses.add(c); if (!whySeenPer.has(src)) whySeenPer.set(src, x.sig); }
    for (const x of r.blind) if (x.sig) for (const c of x.sig.split(" + ")) whyClasses.add(c);
    soleSettled += r.sole.settled; soleUnsettled += r.sole.unsettled; mutDemoted += r.mutFloor.demoted;
    /* THE BRANCH COLUMN IS ARMED HERE AND NOWHERE ELSE. It reads 0 over the corpus, and a column that has
       never spoken cannot tell "the corpus has none" from "the mechanism is dead" — which is exactly the
       pair CLAUDE.md §A-CONTROL-ARMS-ONLY-ON-A-SITE exists to separate. */
    if (src.includes("c?P:")) {
      if (!(r.sites[0] && r.sites[0].alt === "/q"))
        die(`SELF-TEST FAILED: a conditional URL whose arms both fold did not record its second arm, so ` +
            `the branchAlt column is measuring nothing and its zero is unreadable.`);
    }
    if (SELFTEST_GUARDED.has(src) && !(r.sites[0] && r.sites[0].guard > 0))
      die(`SELF-TEST FAILED: a call under an \`if\` was recorded at guard depth 0, so the guard column is ` +
          `measuring nothing.`);
    if (SELFTEST_REACH.has(src)) {
      const want = SELFTEST_REACH.get(src), got = r.sites[0];
      if (!got || got.fnDepth !== want.fnDepth || got.innerAsync !== want.innerAsync)
        die(`SELF-TEST FAILED on the REACH band for \`${src}\`\n  want ${JSON.stringify(want)}\n` +
            `  got  ${JSON.stringify(got && { fnDepth: got.fnDepth, innerAsync: got.innerAsync })}\n` +
            `The band that says whether a door's calls sit in a body something has to CALL is measuring ` +
            `something other than what it prints, so nothing below is printed.`);
      reachSpoke++;
    }
    if (src.startsWith(`const s=`) ) {
      /* THE BASE-RATE CHANNEL IS ARMED TOO. Its whole job is to be the thing a zero above is read against,
         so a base rate that silently stopped counting would leave every zero unreadable. */
      if (!r.pathish.has("/api/looks-like-an-endpoint"))
        die(`SELF-TEST FAILED: an address-shaped literal attached to no door was not counted as a base rate.`);
    }
  }
  /* ── THE CLOSURE'S OWN REFUSALS, EACH SHOWN FIRING ──────────────────────────────────────────────────────
     CLAUDE.md §A-CONTROL-ARMS-ONLY-ON-A-SITE-THE-INSTRUMENT-CAN-JUDGE: a refusal row nobody has watched fire
     cannot be told apart from a refusal row that is dead, so every reason the closure can print is named here
     and the selftest DIES if one of them never spoke. The list is the reasons themselves rather than a count
     of them, because a count would be satisfied by any three of the eight. */
  /* ── THE WHY BAND'S CLASSES, TWO-SIDED ───────────────────────────────────────────────────────────────────
     The forward direction says every class this file DECLARES has been seen; the reverse says every class the
     classifier can EMIT has been declared. Only the reverse catches a class added to `holeClass` and not to
     the list, which is the drift that would leave the band printing a name no reader can look up. */
  for (const [src, want] of SELFTEST_WHY) {
    if (!whySeenPer.has(src))
      die(`SELF-TEST FAILED: the why-band control \`${src}\` produced no door row with a signature, so the ` +
          `class it is the only control for is unarmed.`);
    const got = whySeenPer.get(src);
    if (got !== want)
      die(`SELF-TEST FAILED on the WHY band for \`${src}\`\n  want ${JSON.stringify(want)}\n` +
          `  got  ${JSON.stringify(got)}\nThe class naming WHY an address is incomplete is not the one this ` +
          `control exists to arm, so the band's buckets mean something other than what they print.`);
  }
  for (const k of HOLE_CLASSES)
    if (!whyClasses.has(k))
      die(`SELF-TEST FAILED: no control produces the hole class "${k}", so its bucket is a count nobody has ` +
          `watched rise and a zero in it says nothing about the corpus.`);
  for (const k of whyClasses)
    if (!HOLE_CLASSES.includes(k))
      die(`SELF-TEST FAILED: a control produced the hole class "${k}", which this file does not declare. ` +
          `The declared list has drifted from what \`holeClass\` can emit, so the band would print a name ` +
          `no reader can look up and the two-sided check is the only thing that sees it.`);
  /* ── THE SECOND FIELD ON `a call result`, EACH VERDICT SHOWN FIRING ──────────────────────────────────────── */
  const calleeSeen = new Set();
  for (const [src, want] of SELFTEST_CALLEE) {
    const r = readFile(src, "<selftest>");
    if (!r.parsed) die(`SELF-TEST: the parser refused \`${src}\` — ${r.error}`);
    const got = new Set(r.callResult.verdict.keys());
    for (const v of got) calleeSeen.add(v);
    if (!got.has(want))
      die(`SELF-TEST FAILED on the call-result second field for \`${src}\`\n  want ${JSON.stringify(want)}\n` +
          `  got  ${JSON.stringify([...got])}\nThe verdict this control exists to arm was not produced, so its ` +
          `bucket is a count nobody has watched rise and a zero in it says nothing about the corpus.`);
  }
  for (const v of CALLEE_VERDICTS)
    if (!calleeSeen.has(v))
      die(`SELF-TEST FAILED: no control produces the call-result verdict "${v}", so its bucket is unarmed.`);
  const paramSeen = new Set();
  for (const [src, want] of SELFTEST_CALLEE_PARAM) {
    const r = readFile(src, "<selftest>");
    if (!r.parsed) die(`SELF-TEST: the parser refused \`${src}\` — ${r.error}`);
    const got = r.callResult.rows.filter((x) => x.v === CALLEE_NOT_TEXT).map((x) => x.param);
    if (!got.length)
      die(`SELF-TEST FAILED: the parameter-bit control \`${src}\` resolved no callee, so the bit that sorts ` +
          `this census's rows into two different next diffs is unarmed in one direction.`);
    for (const b of got) paramSeen.add(b);
    if (!got.includes(want))
      die(`SELF-TEST FAILED on the parameter bit for \`${src}\`\n  want ${want}\n  got  ${JSON.stringify(got)}\n` +
          `The split this census prints would then name the wrong population as already covered by the manifest.`);
  }
  for (const b of [true, false])
    if (!paramSeen.has(b))
      die(`SELF-TEST FAILED: no control produces parameter-dependence ${b}, so that side of the split is a ` +
          `count nobody has watched rise.`);
  for (const v of calleeSeen)
    if (!CALLEE_VERDICTS.includes(v))
      die(`SELF-TEST FAILED: a control produced the call-result verdict "${v}", which this file does not ` +
          `declare. The declared list has drifted from what \`calleeVerdict\` can emit, so the census would ` +
          `print a name no reader can look up and the two-sided check is the only thing that sees it.`);
  const wantSoleRefusals = [
    "no call site: it is referenced 2 times — a shared helper",
    "no call site: its one reference is not a callee",
    "no call site: it is not a named value at all — it is the value of a member write",
    "no call site: it is not a named value at all — it is a CLASS METHOD — reached through a receiver",
    "no call site: it is not a named value at all — it is an OBJECT METHOD — reached through a receiver",
    "no call site: it is not a named value at all — it is a CALLBACK argument — its parameter is bound by the callee's own semantics",
    "no call site: it is named and never referenced",
    "no call site: its binding is reassigned",
    "a cyclic call graph",
    "a spread argument",
    "the call supplies no argument at that position",
  ];
  for (const k of wantSoleRefusals)
    if (!soleRefused.has(k))
      die(`SELF-TEST FAILED: no control makes the unique-call-site closure refuse for the reason ` +
          `"${k}", so that row's count is unarmed and a zero in it says nothing about the corpus.`);
  if (!(soleSettled > 0))
    die(`SELF-TEST FAILED: no control shows the unique-call-site closure SETTLING a parameter, so its ` +
        `\`settled 0\` over the corpus cannot be told from a dead mechanism.`);
  if (!(soleUnsettled > 0))
    die(`SELF-TEST FAILED: no control shows the closure crossing to an argument that is itself opaque, so ` +
        `\`unsettled\` — the arm working and buying nothing — is measuring nothing.`);
  if (!(mutDemoted > 0))
    die(`SELF-TEST FAILED: no control demotes a constructed URL read through a name, so the mutable-fold ` +
        `floor's count is unarmed and the residual it prices is a claim rather than a measurement.`);
  const seenBlindKind = new Set(), seenBlindProp = new Set();
  let blindSpoke = 0;
  for (const [src, want] of SELFTEST_BLIND) {
    const r = readFile(src, "<selftest-blind>");
    if (!r.parsed) die(`SELF-TEST: the parser refused \`${src}\` — ${r.error}`);
    const got = r.blind.map((x) => `${x.prop}|${x.kind}|${x.url}`);
    if (got.join("\n") !== want.join("\n"))
      die(`SELF-TEST FAILED on the blind-spot channel for \`${src}\`\n  want ${JSON.stringify(want)}\n` +
          `  got  ${JSON.stringify(got)}\nThe number every PROGRAM-door zero is read against is measuring ` +
          `something other than what it is printed as meaning, so nothing is printed.`);
    if (want.length && r.sites.length)
      die(`SELF-TEST FAILED: \`${src}\` produced ${r.sites.length} SITE row(s). A blind-spot row must never ` +
          `enter the door totals — it is the size of what the doors cannot see, not a door.`);
    for (const x of r.blind) { seenBlindKind.add(x.kind); seenBlindProp.add(x.prop); blindSpoke++; }
  }
  for (const k of ["literal", "folded", "shape", "opaque"])
    if (!seenBlindKind.has(k))
      die(`SELF-TEST FAILED: no control exercises the ${k} kind of the blind-spot channel.`);
  for (const p of ["src", "href"])
    if (!seenBlindProp.has(p)) die(`SELF-TEST FAILED: no control exercises the ${p} blind-spot property.`);
  if (blindSpoke < 5) die(`SELF-TEST FAILED: only ${blindSpoke} blind-spot row(s) from the positive controls.`);
  /* THE MANIFEST CHANNEL, SHAPE BY SHAPE AND REFUSAL BY REFUSAL. */
  let manifestSpoke = 0, manifestAddrs = 0, manifestFrags = 0;
  for (const [src, want] of SELFTEST_MANIFEST) {
    const r = readFile(src, "<selftest-manifest>");
    if (!r.parsed) die(`SELF-TEST: the parser refused \`${src}\` — ${r.error}`);
    const rows = r.manifest.rows;
    if (!want) {
      if (rows.length)
        die(`SELF-TEST FAILED: \`${src}\` produced ${rows.length} manifest row(s) naming ` +
            `${JSON.stringify(rows[0].addresses)}. This shape cannot be folded soundly, so every address ` +
            `above is one this file INVENTED — the single failure the enumeration may not have.`);
      continue;
    }
    if (rows.length !== 1)
      die(`SELF-TEST FAILED: \`${src}\` produced ${rows.length} manifest row(s) and not 1.`);
    const got = { addresses: [...rows[0].addresses].sort(), candidates: rows[0].candidates,
                  dropped: rows[0].dropped, fragments: rows[0].fragments };
    const wantSorted = { ...want, addresses: [...want.addresses].sort(), fragments: want.fragments || 0 };
    if (JSON.stringify(got) !== JSON.stringify(wantSorted))
      die(`SELF-TEST FAILED on the manifest channel for \`${src}\`\n  want ${JSON.stringify(wantSorted)}\n` +
          `  got  ${JSON.stringify(got)}\nThe address set this file reports as what a PARSE recovers is not ` +
          `what its own controls say it is, so nothing is printed.`);
    if (r.sites.length)
      die(`SELF-TEST FAILED: \`${src}\` produced ${r.sites.length} SITE row(s). A manifest row must never ` +
          `enter the door totals — it is an ADDRESS a parse recovers, not a request site.`);
    if (rows[0].addresses.length) manifestSpoke++;
    manifestAddrs += rows[0].addresses.length;
    if (rows[0].fragments) manifestFrags += rows[0].fragments;
  }
  if (manifestSpoke < 2) die(`SELF-TEST FAILED: only ${manifestSpoke} manifest control(s) recovered an address.`);
  if (manifestFrags < 3)
    die(`SELF-TEST FAILED: the FRAGMENT channel counted ${manifestFrags}. It is what separates a chunk ` +
        `manifest from a label table, so a zero it has never been seen to move cannot be read at all.`);
  if (manifestAddrs < 4) die(`SELF-TEST FAILED: the manifest controls enumerated only ${manifestAddrs} address(es).`);
  {
    const r = readFile(SELFTEST_MANIFEST_TWO, "<selftest-manifest>");
    if (r.manifest.rows.length)
      die(`SELF-TEST FAILED: a composition holding TWO applications produced a manifest row. Two unknown ` +
          `parameters make the address set a product of two domains and most of that product does not exist.`);
    if (r.manifest.refusedTwoApplications < 1)
      die(`SELF-TEST FAILED: the two-application refusal is not counted, so its zero over the corpus cannot ` +
          `be told from a shape the scanner never met.`);
  }
  /* THE GLOBAL-REACHED DOOR'S THREE COUNTERS ARE ARMED ONE AT A TIME AND EACH IS SHOWN RISING, because a
     counter whose zero nobody has ever seen move cannot tell "the corpus has none of these" from "this
     channel stopped counting" — and the three are the whole price of the widening, so a dead one turns a
     measured trade back into an assertion. Each row also asserts the OTHER two stay at zero, which is what
     stops one input being read as evidence for a counter it never touched. */
  const gd = (src) => readFile(src, "<selftest>").globalDoor;
  const gdWant = [
    [`window.fetch("/api/a")`,                        { admitted: 1, refusedBoundName: 0, declinedNonGlobalReceiver: 0 }],
    [`new self.Worker("/w.js")`,                      { admitted: 1, refusedBoundName: 0, declinedNonGlobalReceiver: 0 }],
    [`function f(window){return window.fetch("/x")}`, { admitted: 0, refusedBoundName: 1, declinedNonGlobalReceiver: 0 }],
    /* THE WIDENING THE SCOPE PRE-PASS BOUGHT, ARMED IN ONE FILE SO BOTH SIDES MUST SPEAK AT ONCE. The UMD
       factory's parameter shadows `window` INSIDE it and nowhere else, so the reference in the wrapper is
       refused and the one outside is admitted — and a file-wide test cannot produce this pair, because it
       refused both. If a later diff keys the global test on a name again, this row is what fails. */
    [`function f(window){return window.fetch("/x")};window.fetch("/y")`,
                                                      { admitted: 1, refusedBoundName: 1, declinedNonGlobalReceiver: 0 }],
    [`api.fetch("/x")`,                               { admitted: 0, refusedBoundName: 0, declinedNonGlobalReceiver: 1 }],
    [`new p.Worker("/w.js")`,                         { admitted: 0, refusedBoundName: 0, declinedNonGlobalReceiver: 1 }],
    /* A PROPERTY NAMING NO DOOR MUST TOUCH NO COUNTER AT ALL, or the declined figure becomes a count of
       every member call in the corpus and the precision it is printed as pricing is unreadable. */
    [`api.load("/x")`,                                { admitted: 0, refusedBoundName: 0, declinedNonGlobalReceiver: 0 }],
    [`window.open("/x","_blank")`,                    { admitted: 0, refusedBoundName: 0, declinedNonGlobalReceiver: 0 }],
  ];
  for (const [src, want] of gdWant) {
    const got = gd(src);
    for (const k of Object.keys(want))
      if (got[k] !== want[k])
        die(`SELF-TEST FAILED: globalDoor.${k} is ${got[k]} and not ${want[k]} for \`${src}\`. The three ` +
            `numbers that price the global-reached door against what it declines are measuring something ` +
            `other than what they are printed as meaning.`);
  }
  /* THE SPELLING BAND IS ARMED ONE COLUMN AT A TIME, AND THE NEGATIVE ROWS ARE THE HALF THAT MATTERS. A
     probe whose expected output is silence, with no run in which the same probe shape SPOKE, has calibrated
     nothing — and here the silence has to be shown in BOTH directions, because the column that decides the
     verdict is a count of sites where a bare reference is ABSENT. A classifier that quietly counted an object
     key or a parameter as a bare reference would report the property spelling as harmless; one that quietly
     missed a bare reference would report it as critical. Each row therefore states the WHOLE tally, so a
     column rising that should not have is a failure exactly as a column staying flat is. */
  const sp = (src) => {
    const r = readFile(src, "<selftest>");
    const flat = spellTally();
    for (const t of r.spell.values()) for (const k of Object.keys(t)) flat[k] += t[k];
    flat.globalComputedDynamic = r.spellOther.globalComputedDynamic;
    return flat;
  };
  const spWant = [
    [`fetch("/a")`,                             { bareFree: 1 }],
    [`typeof requestAnimationFrame`,            { bareFree: 1, typeofBare: 1 }],
    [`setTimeout(f,0);setInterval(g,1)`,        { bareFree: 2 }],
    [`window.fetch("/a")`,                      { qualified: 1 }],
    [`self.requestIdleCallback(f)`,             { qualified: 1 }],
    [`window["fetch"]("/a")`,                   { computedLiteral: 1 }],
    [`var {fetch} = window`,                    { destructuredFromGlobal: 1 }],
    [`var fetch = 1; fetch("/a")`,              { bareBoundName: 1 }],
    [`function f(window){return window.fetch()}`, { qualifiedBoundGlobal: 1 }],
    [`api.fetch("/a")`,                         { instanceMember: 1 }],
    [`api["setTimeout"](f)`,                    { instanceMemberComputed: 1 }],
    [`self[k]()`,                               { globalComputedDynamic: 1 }],
    /* AND THE SILENCES. An object key, a shorthand property, a parameter, a declared function's own name and
       a default value's binding are not references to the platform, and a channel that counted one would
       report the bare spelling as commoner than it is — which reads as "the floor is harmless". */
    [`({fetch: 1, setTimeout: 2})`,             {}],
    [`function f(fetch, setInterval){}`,        {}],
    [`function setTimeout(){}`,                 {}],
    [`class C { fetch(){} }`,                   {}],
    [`x.notADeclaredName(1)`,                   {}],
    /* A DEFAULT VALUE IS AN ORDINARY EXPRESSION AND THE PARAMETER BESIDE IT IS A BINDING, which is the one
       pattern position a blanket mark would have struck out. */
    [`function f(a = fetch()){}`,               { bareFree: 1 }],
  ];
  let spSpoke = 0;
  for (const [src, want] of spWant) {
    const got = sp(src);
    for (const k of Object.keys(got)) {
      const w = want[k] || 0;
      if (got[k] !== w)
        die(`SELF-TEST FAILED: the spelling band's ${k} is ${got[k]} and not ${w} for \`${src}\`. The column ` +
            `that decides whether a member-name channel is on the critical path is a count of sites with NO ` +
            `bare reference, so a miscount in either direction inverts the verdict this band exists to give.`);
      if (got[k]) spSpoke++;
    }
  }
  if (spSpoke < SPELLINGS.length + 2)
    die(`SELF-TEST FAILED: only ${spSpoke} spelling column(s) were seen to rise; every one of the ` +
        `${SPELLINGS.length} spellings plus typeof and the unattributable computed read must be shown moving, ` +
        `or its zero over the corpus cannot be told from a column that stopped counting.`);
  for (const k of [...SPELLINGS, "typeofBare"]) {
    let rose = false;
    for (const [src] of spWant) if (sp(src)[k]) { rose = true; break; }
    if (!rose) die(`SELF-TEST FAILED: no control makes the spelling band's ${k} rise, so it is unarmed.`);
  }
  /* THE DERIVATION ITSELF IS ARMED, POSITIVE ROW FIRST AND THEN EVERY REFUSAL, because a name the engine
     declares and this pass failed to resolve would leave the population smaller than it is — the flattering
     direction for the diff being priced — and a refusal nobody has seen fire cannot be told from a shape the
     reader never met. The engine tree cannot be broken to test the walk, so the per-source reader is a pure
     function of its text and these are strings. */
  {
    const got = new Map();
    const n = entryNamesFromSource(
      `static const char A_NAME[] = "setTimeout";\n` +
      `static const char *const T_NAMES[] = { A_NAME, "requestIdleCallback", NULL };\n` +
      `rung_entry_declare(STEP_UNIT_TIMER, T_NAMES);\n` +
      `endpoint_fetch_edge_declare("fetch", steps, IDL_STEP_FIRST);\n`,
      "<selftest>", (nm, who) => got.set(nm, who));
    if (n.rung !== 1 || n.edge !== 1 || got.size !== 3 || !got.has("setTimeout") ||
        !got.has("requestIdleCallback") || !got.has("fetch"))
      die(`SELF-TEST FAILED: the entry-name derivation read ${got.size} name(s) from ${n.rung} rung and ` +
          `${n.edge} edge declaration(s) in a source carrying three, so the population this band measures is ` +
          `assembled by something other than what it is printed as reading.`);
    const refusals = [
      [`rung_entry_declare(STEP_UNIT_TIMER, NO_SUCH_TABLE);`, "a table with no declaration"],
      [`static const char *const T[] = { MISSING_ELEM, NULL };\nrung_entry_declare(STEP_UNIT_TIMER, T);`,
       "an element resolving to no literal"],
      [`static const char *const T[] = { NULL };\nrung_entry_declare(STEP_UNIT_TIMER, T);`, "an empty table"],
    ];
    for (const [bad, what] of refusals) {
      let threw = false;
      try { entryNamesFromSource(bad, "<selftest>", () => {}); } catch { threw = true; }
      if (!threw)
        die(`SELF-TEST FAILED: the entry-name derivation accepted ${what} without throwing. A shape change ` +
            `would then reduce the measured population silently, and a smaller population reads as a smaller ` +
            `floor under the very row this band is here to price.`);
    }
  }
  if (!ENTRY_NAMES.size) die(`SELF-TEST FAILED: no declared entry name was derived from the engine.`);
  for (const nm of ENTRY_NAMES)
    if (!sp(`${nm}(f)`).bareFree && !sp(`window.${nm}(f)`).qualified)
      die(`SELF-TEST FAILED: the declared name ${nm} is matched by neither spelling, so it is in the ` +
          `population and outside the channel.`);
  /* THE xhr.open EXCLUSION IS A DECLARED FLOOR AND CARRIES A SIZE FOR THE SAME REASON. */
  if (readFile(`x.open(method,"/t")`, "<selftest>").xhrOpenSkippedNonLiteralMethod !== 1)
    die(`SELF-TEST FAILED: the xhr.open non-literal-method exclusion is not counted, so the floor this ` +
        `file's own DOORS comment says is "reported as xhrOpenSkippedNonLiteralMethod" is a sentence with ` +
        `no number behind it.`);
  for (const k of ["literal", "folded", "shape", "opaque"])
    if (!seenKind.has(k)) die(`SELF-TEST FAILED: no control exercises the ${k} kind, so its count is unarmed.`);
  for (const c of ["data", "program"])
    if (!seenCls.has(c)) die(`SELF-TEST FAILED: no control exercises the ${c} destination class.`);
  /* A CONTROL THAT NEVER SPOKE IS NOT A CONTROL. */
  if (spoke < 10) die(`SELF-TEST FAILED: only ${spoke} row(s) were produced by the positive controls.`);
  if (reachSpoke !== SELFTEST_REACH.size)
    die(`SELF-TEST FAILED: ${reachSpoke} of ${SELFTEST_REACH.size} REACH controls were judged — a control ` +
        `whose source stopped producing a door row is a control that certifies nothing.`);
  return { rows: SELFTEST.length, produced: spoke, blindRows: SELFTEST_BLIND.length, blindProduced: blindSpoke,
           manifestRows: SELFTEST_MANIFEST.length + 1, manifestProduced: manifestSpoke, manifestAddrs,
           reachRows: SELFTEST_REACH.size, spellRows: spWant.length, spellColumns: SPELLINGS.length + 2,
           spellNames: ENTRY_NAMES.size, derivationRefusals: 3,
           soleReasons: wantSoleRefusals.length, soleSettled, soleUnsettled, mutDemoted,
           whyClasses: HOLE_CLASSES.length, whyControls: SELFTEST_WHY.size,
           calleeVerdicts: CALLEE_VERDICTS.length, calleeControls: SELFTEST_CALLEE.size,
           calleeParamControls: SELFTEST_CALLEE_PARAM.size };
}

/* ── THE RUN ──────────────────────────────────────────────────────────────────────────────────────────── */

function main(argv) {
  const arg = (k, d) => { const i = argv.indexOf(k); return i >= 0 && argv[i + 1] ? argv[i + 1] : d; };
  const corpusDir = resolve(arg("--corpus", "engine/.work/sitecorpus/mirror"));
  const wantJson = argv.includes("--json");
  const onlySite = arg("--site", null);
  const nExamples = parseInt(arg("--examples", "0"), 10) || 0;
  const wantManifestUrls = argv.includes("--manifest-urls");

  const st = selftest();

  let stat = null;
  try { stat = statSync(corpusDir); } catch { /* handled below */ }
  if (!stat || !stat.isDirectory())
    die(`no corpus at ${corpusDir}. THIS REPOSITORY CARRIES NO COPY OF ANYBODY ELSE'S SITE — the driver is ` +
        `tracked and the corpus is not (testing/corpus/README.md), so there is nothing here to fall back ` +
        `on and this is not a defect. Make one, then re-run:\n` +
        `    NODE_USE_ENV_PROXY=1 SITES=apps.tsv node testing/corpus/fetch.mjs\n` +
        `    node testing/static_surface.mjs --corpus ${corpusDir}`);

  /* CALIBRATION FIRST. `corpusPrograms` publishes its own totals, so this probe reproduces them before any
     breakdown of them is believed — the disagreement is the finding rather than the breakdown. */
  const cp = corpusPrograms(corpusDir, TAG);

  /* SITE ATTRIBUTION BY CONTENT. The manifest's row carries the site id; the digest carries the file. A
     path rule here would be the second copy engine/corpus_programs.mjs refuses by name. */
  const manifestPath = resolve(corpusDir, "..", "provenance.json");
  const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
  const rows = Array.isArray(manifest) ? manifest : Object.values(manifest);
  const siteOf = new Map(), essOf = new Map();
  let fetchedFrom = null, fetchedTo = null;
  const note = (sha, id, ct, at) => {
    if (!sha) return;
    if (!siteOf.has(sha)) siteOf.set(sha, new Set());
    siteOf.get(sha).add(id);
    essOf.set(sha, essenceOf(ct));
    if (at) { if (!fetchedFrom || at < fetchedFrom) fetchedFrom = at; if (!fetchedTo || at > fetchedTo) fetchedTo = at; }
  };
  for (const r of rows) {
    note(r.sha256, r.id, r.contentType, r.fetchedAt);
    for (const s of r.resources || []) note(s.sha256, r.id, s.contentType, s.fetchedAt);
  }

  const sha = (b) => createHash("sha256").update(b).digest("hex");
  /* ONE TALLY SHAPE FOR BOTH CLASSES, so the two can only ever be printed the same way and a reader
     comparing them is comparing like with like. */
  const kindTally = () => ({ sites: 0, literal: 0, folded: 0, shape: 0, opaque: 0, guarded: 0,
                            /* THE REACH BAND, PER CLASS, so the two door classes are comparable on it —
                               which is the whole point: the question is not how deep a call sits but
                               whether the two classes sit in the SAME KIND of body. */
                            topLevel: 0, innerAsync: 0, innerSync: 0, urls: new Set() });
  const perSite = new Map();
  const bucket = (id) => {
    if (!perSite.has(id)) perSite.set(id, {
      site: id, programs: 0, bytes: 0, parsed: 0, unparsed: 0, sites: 0,
      literal: 0, folded: 0, shape: 0, opaque: 0, guarded: 0, branchAlt: 0,
      engineDoorSites: 0, nonEngineDoorSites: 0, byDoor: {}, pathish: new Set(), urls: new Set(), rows: [],
      data: kindTally(), program: kindTally(),
      argShape: {}, bindBuckets: { "1": 0, "2-5": 0, "6-20": 0, "21-100": 0, "101+": 0, "0": 0 }, oneCharNames: 0,
      blind: { sites: 0, literal: 0, folded: 0, shape: 0, opaque: 0, src: 0, href: 0 }, blindUrls: new Set(), xhrOpenSkipped: 0,
      globalDoor: { admitted: 0, refusedBoundName: 0, declinedNonGlobalReceiver: 0 },
      inline: { settled: 0, refused: new Map() }, recvDoor: { admitted: 0, declined: 0 },
      sole: { settled: 0, unsettled: 0, refused: new Map() }, mutFloor: { demoted: 0, rows: [] },
      callResult: { verdict: new Map(), rows: [] },
      sig: { data: new Map(), program: new Map(), blind: new Map() },
      manifest: { sites: 0, addressSites: 0, candidates: 0, dropped: 0, fragments: 0, refusedTwoApplications: 0, multi: 0 },
      manifestUrls: new Set(), manifestRows: [],
      spell: {}, spellOther: { globalComputedDynamic: 0 },
    });
    return perSite.get(id);
  };

  let nProgramSeen = 0, nDocumentSeen = 0, ambiguous = 0, parseFail = [];
  const t0 = Date.now();

  for (const f of cp.files) {
    const buf = readFileSync(f);
    const d = sha(buf);
    const ess = essOf.get(d);
    if (DOCUMENT.has(ess)) { nDocumentSeen++; continue; }
    if (!PROGRAM.has(ess)) die(`${relative(corpusDir, f)} is neither program nor document by the manifest ` +
                               `(${JSON.stringify(ess)}) — corpusPrograms should have refused it first.`);
    nProgramSeen++;
    const ids = siteOf.get(d);
    if (!ids || ids.size === 0) die(`no manifest row names the site of ${relative(corpusDir, f)}`);
    if (ids.size > 1) ambiguous++;
    const id = [...ids].sort()[0];
    if (onlySite && id !== onlySite) continue;

    const b = bucket(id);
    b.programs++; b.bytes += buf.length;
    const r = readFile(buf.toString("utf8"), relative(corpusDir, f));
    if (!r.parsed) { b.unparsed++; parseFail.push(`${relative(corpusDir, f)}: ${r.error}`); continue; }
    b.parsed++;
    for (const v of r.pathish) b.pathish.add(v);
    b.xhrOpenSkipped += r.xhrOpenSkippedNonLiteralMethod;
    for (const k of Object.keys(b.globalDoor)) b.globalDoor[k] += r.globalDoor[k];
    for (const k of Object.keys(b.recvDoor)) b.recvDoor[k] += r.recvDoor[k];
    b.inline.settled += r.inline.settled;
    for (const [k, v] of r.inline.refused) b.inline.refused.set(k, (b.inline.refused.get(k) || 0) + v);
    b.sole.settled += r.sole.settled; b.sole.unsettled += r.sole.unsettled;
    for (const [k, v] of r.sole.refused) b.sole.refused.set(k, (b.sole.refused.get(k) || 0) + v);
    b.mutFloor.demoted += r.mutFloor.demoted;
    for (const row of r.mutFloor.rows) b.mutFloor.rows.push(row);
    for (const [k, v] of r.callResult.verdict) b.callResult.verdict.set(k, (b.callResult.verdict.get(k) || 0) + v);
    for (const row of r.callResult.rows) b.callResult.rows.push(row);
    /* THE BAND'S BUCKETS. Only an INCOMPLETE row has a signature, so this counts exactly the population the
       razor is about and the assertion below can say so. */
    for (const x of r.sites) if (x.sig) { const m = b.sig[x.cls]; m.set(x.sig, (m.get(x.sig) || 0) + 1); }
    for (const x of r.blind) if (x.sig) b.sig.blind.set(x.sig, (b.sig.blind.get(x.sig) || 0) + 1);
    b.spellOther.globalComputedDynamic += r.spellOther.globalComputedDynamic;
    for (const [nm, t] of r.spell) {
      if (!b.spell[nm]) b.spell[nm] = spellTally();
      for (const k of Object.keys(t)) b.spell[nm][k] += t[k];
    }
    b.manifest.refusedTwoApplications += r.manifest.refusedTwoApplications;
    for (const m of r.manifest.rows) {
      b.manifest.sites++;
      if (m.addresses.length) b.manifest.addressSites++;
      b.manifest.candidates += m.candidates;
      b.manifest.dropped += m.dropped;
      b.manifest.fragments += m.fragments;
      if (m.addresses.length > 1) b.manifest.multi++;
      for (const u of m.addresses) b.manifestUrls.add(u);
      /* THE RECOVERED SET ITSELF, ON ASK, BECAUSE THIS CHANNEL'S SOUNDNESS CLAIM IS OTHERWISE UNCHECKABLE
         FROM ITS OWN OUTPUT. Every other number here is a count a reader can argue with; an ADDRESS SET is a
         claim that these particular strings are addresses the bundle can load, and printing only its
         cardinality is a contract that names a hazard and offers no exit. With this flag a reader can run the
         runtime's own chunk-URL function over its own key domain and compare the two sets, which is what
         established the channel is exact rather than merely plausible: on the corpus this was last run
         against, one site's 1176 recovered addresses are BYTE-IDENTICAL to the image of that function, and 4
         of them name files the fetcher independently mirrored. */
      if (wantManifestUrls) for (const u of m.addresses) console.log(`MANIFEST-URL\t${id}\t${m.file}:${m.line}\t${u}`);
      if (nExamples) b.manifestRows.push(m);
    }
    for (const s of r.blind) {
      b.blind.sites++; b.blind[s.kind]++; b.blind[s.prop]++;
      if (s.kind !== "opaque") b.blindUrls.add(s.url);
    }
    for (const s of r.sites) {
      b.sites++;
      b[s.kind]++;
      if (s.guard > 0) b.guarded++;
      if (s.alt) b.branchAlt++;
      if (s.engineDoor) b.engineDoorSites++; else b.nonEngineDoorSites++;
      b.byDoor[s.door] = (b.byDoor[s.door] || 0) + 1;
      const c = b[s.cls];
      c.sites++; c[s.kind]++; if (s.guard > 0) c.guarded++;
      /* AN EXACT PARTITION OF `c.sites` AND NOT THREE INDEPENDENT COUNTS, asserted below at the one place
         all four are in one hand: a row is at depth 0 or it is not, and if it is not its innermost
         enclosing function is async or it is not. A reader differencing two of the three would otherwise be
         differencing quantities nothing holds together. */
      if (s.fnDepth === 0) c.topLevel++; else if (s.innerAsync) c.innerAsync++; else c.innerSync++;
      /* THE CEILING COLUMNS ARE DATA-DOOR ONLY AND ONLY OVER ROWS THE FOLD DID NOT SETTLE, because that is
         the population the "a better parser would get these" objection is about; counting settled rows in
         it would answer a question nobody asked.
         THE TEST USED TO BE `kind !== "literal"`, WHICH IS THE SAME POPULATION ONLY WHILE `folded` IS SMALL.
         A folded row IS settled — its whole text was recovered without running anything — so counting it
         here asks how much better a stronger parser could do on rows this file ALREADY DID. That proxy was
         written when the fold resolved a name only if the whole file bound it once, which on this corpus was
         eight data-door rows; a scope-correct resolver settles twice that, and the gap grows with every
         widening. The population is now the two UNSETTLED kinds by name, which is what the sentence above
         always said it was. */
      if (s.cls === "data" && (s.kind === "shape" || s.kind === "opaque")) {
        b.argShape[s.argShape] = (b.argShape[s.argShape] || 0) + 1;
        if (s.argBinds !== null) {
          const n = s.argBinds;
          const k = n === 0 ? "0" : n === 1 ? "1" : n <= 5 ? "2-5" : n <= 20 ? "6-20" : n <= 100 ? "21-100" : "101+";
          b.bindBuckets[k]++;
          if (s.argNameLen === 1) b.oneCharNames++;
        }
      }
      if (s.kind !== "opaque") { b.urls.add(s.method + " " + s.url); c.urls.add(s.method + " " + s.url); }
      if (nExamples) b.rows.push(s);
    }
  }
  const ms = Date.now() - t0;

  /* THE CALIBRATION IS ASSERTED AND NOT PRINTED-AND-HOPED-FOR. A probe that quietly walks a different
     population than the instrument it reproduces returns a plausible, larger, wrong number. */
  if (!onlySite && nProgramSeen !== cp.nProgram)
    die(`this probe typed ${nProgramSeen} file(s) as programs where corpusPrograms published ` +
        `${cp.nProgram}. The two selectors disagree about the population, which is the finding — do not ` +
        `read anything below this line.`);
  if (!onlySite && nDocumentSeen !== cp.nDocument)
    die(`this probe typed ${nDocumentSeen} document(s) where corpusPrograms published ${cp.nDocument}.`);

  const listedSites = new Set(rows.map((r) => r.id));
  const missingSites = [...listedSites].filter((id) => !perSite.has(id)).sort();

  const tot = {
    missingSites, corpusSites: listedSites.size,
    corpus: corpusDir, manifest: manifestPath,
    fetchedFrom, fetchedTo, readAt: new Date().toISOString(), ms,
    corpusProgramsSays: { onDisk: cp.onDisk, nProgram: cp.nProgram, nDocument: cp.nDocument, nExcluded: cp.nExcluded, bytes: cp.bytes },
    calibration: onlySite ? "SKIPPED (--site narrows the population)" : "REPRODUCED",
    siteCount: perSite.size, ambiguousBlobs: ambiguous,
    programs: 0, bytes: 0, parsed: 0, unparsed: 0,
    sites: 0, literal: 0, folded: 0, shape: 0, opaque: 0, guarded: 0, branchAlt: 0,
    engineDoorSites: 0, nonEngineDoorSites: 0, byDoor: {}, distinctUrls: 0, pathish: 0,
    argShape: {}, bindBuckets: { "1": 0, "2-5": 0, "6-20": 0, "21-100": 0, "101+": 0, "0": 0 }, oneCharNames: 0,
    blind: { sites: 0, literal: 0, folded: 0, shape: 0, opaque: 0, src: 0, href: 0 }, blindDistinctUrls: 0, xhrOpenSkipped: 0,
    globalDoor: { admitted: 0, refusedBoundName: 0, declinedNonGlobalReceiver: 0 },
    inline: { settled: 0, refused: new Map() }, recvDoor: { admitted: 0, declined: 0 },
    sole: { settled: 0, unsettled: 0, refused: new Map() }, mutFloor: { demoted: 0, rows: [] },
    callResult: { verdict: new Map(), rows: [] },
    sig: { data: new Map(), program: new Map(), blind: new Map() },
    manifest: { sites: 0, addressSites: 0, candidates: 0, dropped: 0, fragments: 0, refusedTwoApplications: 0, multi: 0 }, manifestDistinctUrls: 0,
    /* THE SPELLING BAND'S OWN TOTALS. `spellSites` is a presence count over SITES and never a sum of
       occurrences, because the landed rows it prices are read as a bit and a site is the unit at which one
       of them is zero. */
    spell: {}, spellOther: { globalComputedDynamic: 0 }, spellSites: {},
  };
  const allUrls = new Set(), allPathish = new Set(), allBlindUrls = new Set(), allManifestUrls = new Set();
  const clsUrls = { data: new Set(), program: new Set() };
  tot.data = kindTally(); tot.program = kindTally();
  for (const b of perSite.values()) {
    for (const k of ["programs", "bytes", "parsed", "unparsed", "sites", "literal", "folded", "shape",
                     "opaque", "guarded", "branchAlt", "engineDoorSites", "nonEngineDoorSites"]) tot[k] += b[k];
    for (const [k, v] of Object.entries(b.byDoor)) tot.byDoor[k] = (tot.byDoor[k] || 0) + v;
    for (const [k, v] of Object.entries(b.argShape)) tot.argShape[k] = (tot.argShape[k] || 0) + v;
    for (const k of Object.keys(tot.bindBuckets)) tot.bindBuckets[k] += b.bindBuckets[k];
    tot.oneCharNames += b.oneCharNames;
    for (const cls of ["data", "program"]) {
      for (const k of ["sites", "literal", "folded", "shape", "opaque", "guarded",
                       "topLevel", "innerAsync", "innerSync"]) tot[cls][k] += b[cls][k];
      for (const u of b[cls].urls) clsUrls[cls].add(u);
    }
    for (const u of b.urls) allUrls.add(u);
    for (const p of b.pathish) allPathish.add(p);
    for (const k of Object.keys(tot.blind)) tot.blind[k] += b.blind[k];
    tot.xhrOpenSkipped += b.xhrOpenSkipped;
    for (const k of Object.keys(tot.globalDoor)) tot.globalDoor[k] += b.globalDoor[k];
    for (const k of Object.keys(tot.recvDoor)) tot.recvDoor[k] += b.recvDoor[k];
    tot.inline.settled += b.inline.settled;
    for (const [k, v] of b.inline.refused) tot.inline.refused.set(k, (tot.inline.refused.get(k) || 0) + v);
    tot.sole.settled += b.sole.settled; tot.sole.unsettled += b.sole.unsettled;
    for (const [k, v] of b.sole.refused) tot.sole.refused.set(k, (tot.sole.refused.get(k) || 0) + v);
    tot.mutFloor.demoted += b.mutFloor.demoted;
    for (const row of b.mutFloor.rows) tot.mutFloor.rows.push(row);
    for (const [k, v] of b.callResult.verdict) tot.callResult.verdict.set(k, (tot.callResult.verdict.get(k) || 0) + v);
    for (const row of b.callResult.rows) tot.callResult.rows.push(row);
    for (const k of ["data", "program", "blind"])
      for (const [sg, v] of b.sig[k]) tot.sig[k].set(sg, (tot.sig[k].get(sg) || 0) + v);
    tot.spellOther.globalComputedDynamic += b.spellOther.globalComputedDynamic;
    for (const nm of ENTRY_NAMES) {
      const t = b.spell[nm];
      if (!tot.spell[nm]) tot.spell[nm] = spellTally();
      if (!tot.spellSites[nm])
        tot.spellSites[nm] = { any: 0, bareFree: 0, bareAny: 0, propertyAny: 0,
                               propertyOnlyTight: 0, propertyOnlyLoose: 0, wrapperOnly: 0 };
      if (!t) continue;
      for (const k of Object.keys(t)) tot.spell[nm][k] += t[k];
      /* THE PLATFORM-NAME POPULATION IS THE GLOBAL SPELLINGS AND EXCLUDES A WRAPPER'S OWN MEMBER. `api.fetch`
         names a receiver whose identity is in question, which the DOORS comment turns away and which no
         global-resolution channel of any spelling would ever raise — counting it here would inflate the
         population the member channel is being priced against with rows that channel cannot rescue. */
      const bareAny = t.bareFree + t.bareBoundName;
      const propAny = t.qualified + t.qualifiedBoundGlobal + t.computedLiteral + t.destructuredFromGlobal;
      const s = tot.spellSites[nm];
      if (bareAny || propAny) s.any++;
      if (bareAny) s.bareAny++;
      if (t.bareFree) s.bareFree++;
      if (propAny) s.propertyAny++;
      if (propAny && !bareAny) s.propertyOnlyTight++;
      if (propAny && !t.bareFree) s.propertyOnlyLoose++;
      if (!bareAny && !propAny && (t.instanceMember + t.instanceMemberComputed)) s.wrapperOnly++;
    }
    for (const k of Object.keys(tot.manifest)) tot.manifest[k] += b.manifest[k];
    for (const u of b.manifestUrls) allManifestUrls.add(u);
    for (const u of b.blindUrls) allBlindUrls.add(u);
  }
  tot.blindDistinctUrls = allBlindUrls.size;
  tot.manifestDistinctUrls = allManifestUrls.size;
  tot.distinctUrls = allUrls.size; tot.pathish = allPathish.size;
  tot.data.distinctUrls = clsUrls.data.size; tot.program.distinctUrls = clsUrls.program.size;
  delete tot.data.urls; delete tot.program.urls;

  /* THE PARTS SUM TO THE TOTAL, ASSERTED, because a count whose parts cannot be checked against it is a
     count a reader has to take on trust. */
  if (tot.literal + tot.folded + tot.shape + tot.opaque !== tot.sites)
    die(`the kind partition does not sum: ${tot.literal}+${tot.folded}+${tot.shape}+${tot.opaque} ` +
        `!= ${tot.sites}`);
  if (tot.engineDoorSites + tot.nonEngineDoorSites !== tot.sites)
    die(`the door partition does not sum against ${tot.sites}`);
  if (tot.data.sites + tot.program.sites !== tot.sites)
    die(`the destination partition does not sum: ${tot.data.sites}+${tot.program.sites} != ${tot.sites}`);
  for (const cls of ["data", "program"])
    if (tot[cls].literal + tot[cls].folded + tot[cls].shape + tot[cls].opaque !== tot[cls].sites)
      die(`the ${cls} kind partition does not sum against ${tot[cls].sites}`);
  /* AND THE REACH BAND IS A PARTITION OF THE SAME `sites`, WHICH IS WHAT MAKES IT DIFFERENCEABLE. Without
     this a reader comparing `innerAsync` across the two classes would be comparing two numbers nothing
     holds to one denominator, and a row silently dropped out of all three would read as a class with
     fewer async sites — the flattering direction for the DATA door and the one that would make its zero in
     a run look explained. */
  for (const cls of ["data", "program"])
    if (tot[cls].topLevel + tot[cls].innerAsync + tot[cls].innerSync !== tot[cls].sites)
      die(`the ${cls} reach partition does not sum: ${tot[cls].topLevel}+${tot[cls].innerAsync}+` +
          `${tot[cls].innerSync} != ${tot[cls].sites}`);
  if (tot.blind.literal + tot.blind.folded + tot.blind.shape + tot.blind.opaque !== tot.blind.sites)
    die(`the blind-spot kind partition does not sum against ${tot.blind.sites}`);
  if (tot.blind.src + tot.blind.href !== tot.blind.sites)
    die(`the blind-spot property partition does not sum against ${tot.blind.sites}`);

  const out = {
    total: tot,
    perSite: [...perSite.values()].sort((a, b) => b.data.sites - a.data.sites || b.sites - a.sites).map((b) => ({
      site: b.site, programs: b.programs, bytes: b.bytes, parsed: b.parsed, unparsed: b.unparsed,
      sites: b.sites, literal: b.literal, folded: b.folded, shape: b.shape, opaque: b.opaque,
      guarded: b.guarded, branchAlt: b.branchAlt, engineDoorSites: b.engineDoorSites,
      nonEngineDoorSites: b.nonEngineDoorSites, byDoor: b.byDoor,
      distinctUrls: b.urls.size, pathish: b.pathish.size,
      argShape: b.argShape, bindBuckets: b.bindBuckets, oneCharNames: b.oneCharNames,
      data: { ...b.data, urls: undefined, distinctUrls: b.data.urls.size },
      program: { ...b.program, urls: undefined, distinctUrls: b.program.urls.size },
      blind: { ...b.blind, distinctUrls: b.blindUrls.size }, xhrOpenSkipped: b.xhrOpenSkipped,
      globalDoor: b.globalDoor,
      spell: b.spell, spellOther: b.spellOther,
      manifest: { ...b.manifest, distinctUrls: b.manifestUrls.size },
      manifestAddresses: nExamples ? [...b.manifestUrls].sort() : undefined,
    })),
    parseFailures: parseFail,
    examples: nExamples ? [...perSite.values()].flatMap((b) => b.rows.slice(0, nExamples)) : undefined,
    manifestExamples: nExamples ? [...perSite.values()].flatMap((b) => b.manifestRows.slice(0, 2).map((m) =>
      ({ ...m, addresses: m.addresses.slice(0, nExamples) }))) : undefined,
  };

  if (wantJson) { console.log(JSON.stringify(out, null, 1)); return; }

  const pct = (n, d) => (d ? (100 * n / d).toFixed(1) : "0.0") + "%";
  console.log(`# static_surface — WHAT A PARSE RECOVERS FROM THE JS DOOR. A CONTROL, NEVER A TARGET.`);
  console.log(`selftest ARMED: ${st.rows} controls produced ${st.produced} classified row(s); every kind and ` +
              `both destination classes exercised`);
  console.log(`         plus ${st.blindRows} blind-spot controls producing ${st.blindProduced} row(s), each ` +
              `asserted to enter NO door total`);
  console.log(`         plus ${st.manifestRows} chunk-manifest controls: ${st.manifestProduced} enumerated ` +
              `${st.manifestAddrs} address(es), the rest refused for a stated reason`);
  console.log(`         plus ${st.reachRows} REACH controls, each asserted for BOTH its function depth and ` +
              `whether its innermost enclosing function is async`);
  console.log(`         plus the WHY band: all ${st.whyClasses} hole class(es) shown firing from ${st.whyControls} control(s), ` +
              `checked BOTH ways so a class the classifier emits cannot go undeclared`);
  console.log(`         plus the call-result second field: all ${st.calleeVerdicts} verdict(s) shown firing from ` +
              `${st.calleeControls} control(s) checked BOTH ways, and both sides of the parameter split from ` +
              `${st.calleeParamControls} more — so a ZERO in the credited bucket is a fact about the corpus`);
  console.log(`         plus the unique-call-site closure: all ${st.soleReasons} refusal reason(s) shown FIRING, ` +
              `${st.soleSettled} parameter(s) settled and ${st.soleUnsettled} crossed to an opaque argument, and ` +
              `${st.mutDemoted} mutable-fold demotion(s) — so neither price is a zero nobody has armed`);
  console.log(`         plus ${st.spellRows} SPELLING controls over ${st.spellNames} name(s) derived from the ` +
              `engine, every one of ${st.spellColumns} column(s) shown rising and every silence asserted`);
  console.log(`         plus the derivation itself: 1 positive row and ${st.derivationRefusals} refusal(s) ` +
              `shown THROWING, so a shape change cannot quietly report a smaller population`);
  console.log(`corpus   ${corpusDir}`);
  console.log(`fetched  ${fetchedFrom} .. ${fetchedTo}   read ${tot.readAt}   parse ${ms} ms`);
  console.log(`corpusPrograms: ${cp.onDisk} on disk = ${cp.nProgram} program + ${cp.nDocument} document + ` +
              `${cp.nExcluded} excluded, ${(cp.bytes / 1048576).toFixed(1)} MB  [calibration ${tot.calibration}]`);
  console.log(`parsed   ${tot.parsed}/${tot.programs} programs (${tot.unparsed} refused), ` +
              `${(tot.bytes / 1048576).toFixed(1)} MB over ${tot.siteCount} site(s)`);
  if (tot.missingSites.length)
    console.log(`no program in the corpus for ${tot.missingSites.length} of ${tot.corpusSites} listed site(s): ` +
                `${tot.missingSites.join(", ")} — a fact about the FETCH, not about the parse`);
  if (tot.ambiguousBlobs)
    console.log(`${tot.ambiguousBlobs} blob(s) are shared by more than one site and are attributed to one of them`);
  const block = (name, k, why) => {
    console.log(``);
    console.log(`${name} — ${why}`);
    console.log(`  sites ${k.sites}   distinct addresses ${k.distinctUrls}`);
    console.log(`  literal ${k.literal} (${pct(k.literal, k.sites)})   a string in the text; the parse and the engine both have it`);
    console.log(`  folded  ${k.folded} (${pct(k.folded, k.sites)})   resolved without running anything; the parse has it too`);
    console.log(`  shape   ${k.shape} (${pct(k.shape, k.sites)})   literal text + a hole; the parse has the SHAPE and never the VALUE`);
    console.log(`  opaque  ${k.opaque} (${pct(k.opaque, k.sites)})   no literal text at all; only "a request happens here"`);
    console.log(`  --> complete address from the text at ${pct(k.literal + k.folded, k.sites)}; ` +
                `${k.shape + k.opaque} site(s) (${pct(k.shape + k.opaque, k.sites)}) need a VALUE only a run has.`);
    console.log(`  guarded ${k.guarded} (${pct(k.guarded, k.sites)}) under >=1 test — read by the parse whether the gate is taken or not`);
    console.log(`  reach: top-level ${k.topLevel} (${pct(k.topLevel, k.sites)})   ` +
                `inside an async fn ${k.innerAsync} (${pct(k.innerAsync, k.sites)})   ` +
                `inside a sync fn ${k.innerSync} (${pct(k.innerSync, k.sites)})`);
  };
  block(`DATA DOOR (fetch / XMLHttpRequest / sendBeacon / WebSocket / EventSource)`, tot.data,
        `Fetch §2.2.5 destinations whose reply becomes a VALUE. THIS IS THE @H PRODUCT SURFACE.`);
  block(`PROGRAM DOOR (import() / Worker / SharedWorker / importScripts)`, tot.program,
        `replies that become a PROGRAM — the page loading itself. Reported apart and never summed in.`);
  console.log(``);
  console.log(`WHAT HAS TO BE CALLED FOR A DOOR TO BE REACHED — the two classes compared on the ONE axis they`);
  console.log(`  differ on, which is NOT how deep they sit. A parse reads a call whether anything invokes its`);
  console.log(`  enclosing function or not; a RUN reaches it only if something does. So a door class whose`);
  console.log(`  calls sit at top level is reached by evaluating the program, and one whose calls sit inside an`);
  console.log(`  \`async\` body is reached only once something INVOKES that body — which in a real app is an`);
  console.log(`  effect flushed after a render commit, an event handler, a timer or an idle callback, and is a`);
  console.log(`  different question from whether the program ran at all. THE NUMBERS ARE IN THE TWO BLOCKS`);
  console.log(`  ABOVE, one \`reach:\` line each, so the comparison is read where each class's own denominator`);
  console.log(`  is. This paragraph states what the comparison MEANS and asserts nothing about any engine:`);
  console.log(`  both columns are properties of the TEXT and a run is what decides whether the invoker fires.`);
  console.log(`  IT IS A FLOOR IN ONE DIRECTION ONLY, AND THE DIRECTION IS STATED BECAUSE IT IS NOT SYMMETRIC:`);
  console.log(`  \`innerSync\` over-states reachability (a sync function nothing calls is as unreached as an`);
  console.log(`  async one) while \`innerAsync\` cannot — an async body needs an invoker by construction. So a`);
  console.log(`  high \`innerAsync\` share is evidence the class needs an invoker; a high \`innerSync\` share is`);
  console.log(`  NOT evidence that it does not, and reading it as one is the reading this note exists to stop.`);
  console.log(``);
  console.log(`THE DOOR SET'S OWN BLIND SPOT, MEASURED — \`el.src =\` / \`el.href =\`, which html_script.c and`);
  console.log(`  html_link.c DO record and no door above reads. NOT sites and summed into no total; this is the`);
  console.log(`  number a PROGRAM-door or DATA-door ZERO has to be read against, because a bundler that loads`);
  console.log(`  its chunks by injecting a <script> passes through here and through no door at all.`);
  console.log(`  assignments ${tot.blind.sites}   (.src ${tot.blind.src}  .href ${tot.blind.href})   ` +
              `distinct addresses ${tot.blindDistinctUrls}`);
  console.log(`  literal ${tot.blind.literal} (${pct(tot.blind.literal, tot.blind.sites)})   ` +
              `folded ${tot.blind.folded} (${pct(tot.blind.folded, tot.blind.sites)})   ` +
              `shape ${tot.blind.shape} (${pct(tot.blind.shape, tot.blind.sites)})   ` +
              `opaque ${tot.blind.opaque} (${pct(tot.blind.opaque, tot.blind.sites)})`);
  console.log(`  ${tot.xhrOpenSkipped} further xhr.open call(s) were skipped for a non-literal first argument —`);
  console.log(`  the floor the DOORS comment names, carrying a size rather than a sentence.`);
  console.log(``);
  console.log(`THE GLOBAL-REACHED DOOR AND ITS PRICE — \`window.fetch(u)\` and \`new self.Worker(u)\` reach a`);
  console.log(`  platform name through the global object, and the door set reads them as the bare name because`);
  console.log(`  they ARE the bare name. These three are one trade and are printed together: a recall figure`);
  console.log(`  alone would be a widening whose precision cost nobody measured.`);
  console.log(`  admitted ${tot.globalDoor.admitted}   already counted inside the DATA and PROGRAM totals above, not added to them`);
  console.log(`  refused  ${tot.globalDoor.refusedBoundName}   that reference RESOLVES to a binding this file makes, so it is not the global`);
  console.log(`  declined ${tot.globalDoor.declinedNonGlobalReceiver}   a platform door name on a receiver that is not the global object — the library`);
  console.log(`           wrapper population the DOORS comment turns away, counted rather than described`);
  console.log(``);
  console.log(`THE RECEIVER-QUALIFIED DOOR AND ITS PRICE — \`navigator.serviceWorker.register(u)\` names a`);
  console.log(`  script URL that becomes a PROGRAM, and it is the one door whose PROPERTY alone cannot be`);
  console.log(`  keyed on: a dependency container, an i18n catalogue and a component registry all have a`);
  console.log(`  \`register\`. So the receiver is tested for the platform name, and both halves are printed`);
  console.log(`  because a widening whose precision cost is unmeasured is a trade nobody made. Its engine`);
  console.log(`  column is null, so it is a FLOOR-WIDENING of the static side and not a comparison.`);
  console.log(`  admitted ${tot.recvDoor.admitted}   inside the PROGRAM total above, counted apart from the engine-comparable doors`);
  console.log(`  declined ${tot.recvDoor.declined}   a \`register\` on a receiver that is not the platform name — the population`);
  console.log(`           keying on the property alone would have reported as requests`);
  console.log(``);

  /* ── THE INTERPROCEDURAL INLINE AND ITS PRICE ───────────────────────────────────────────────────────────
     Printed beside the global door's three numbers because it is the same KIND of trade and is read the same
     way: what a widening ADMITTED means nothing without what it DECLINED. `settled` is the number of calls
     whose whole value this fold recovered by crossing into the callee with the argument the call site
     supplies; the refusals are the shapes it would not cross, each for a reason stated at its own arm rather
     than as a policy here. A refusal count that is LARGER than `settled` is the expected reading and not a
     failure: the callee of a minified member call is a slot no parse can join to its writes, and saying so
     with a size is what makes the settled figure an honest floor instead of a total. */
  console.log(``);
  console.log(`THE INTERPROCEDURAL INLINE AND WHAT IT DECLINED — the door channel crosses a function`);
  console.log(`  boundary at a call whose ARGUMENTS this fold already settled, binding the value the call`);
  console.log(`  site really passes rather than a candidate drawn from the callee. It is the direction the`);
  console.log(`  chunk-manifest band crosses the same boundary in, and it is reported the same way: a`);
  console.log(`  widening whose refusals are unmeasured is a trade nobody made.`);
  console.log(`  settled ${tot.inline.settled}   call(s) whose whole value the inline recovered`);
  for (const [k, v] of [...tot.inline.refused].sort((a, b) => b[1] - a[1]))
    console.log(`  refused ${String(v).padStart(5)}   ${k === "async" ? "an `async` or generator callee — its call evaluates to a Promise and not to the body's value, so inlining one would fabricate an address that looks exactly like the real one" : k === "nested-fn" ? "a function written inside the expression being inlined, which could rebind the parameter's name and make the substitution name a value no call site passes" : k}`);
  /* ── THE UNIQUE-CALL-SITE CLOSURE AND ITS PRICE ─────────────────────────────────────────────────────────
     THE OTHER DIRECTION OF THE BOUNDARY THE INLINE CROSSES, and reported the same way for the same reason.
     What this one adds that the inline's block does not is that its refusals name a STRUCTURE rather than a
     spelling: a helper referenced five times is a shared helper and no call-site analysis will ever settle
     its parameters, while a function that is not a named value AT ALL — an object-literal property, a
     callback argument, a returned closure — is the bundler's module shape, where there is no reference for
     any count to be about. Those two take opposite work, and summing them into one number would say only
     that the closure declined.
     `unsettled` IS PRINTED APART FROM `settled` BECAUSE THEY ARE DIFFERENT FACTS. A parameter resolved to an
     argument that is itself opaque is this arm WORKING and buying nothing — the caller did not spell the
     address either — and that is not the arm refusing. A reading that merged them would report the closure
     as having declined a population it in fact crossed. */
  console.log(``);
  console.log(`THE UNIQUE-CALL-SITE CLOSURE AND WHAT IT DECLINED — a door written inside a helper reads its`);
  console.log(`  address from a PARAMETER, which has no initializer and no slot, so the row is opaque however`);
  console.log(`  completely the CALLER spelled the address. Where a function's binding is referenced exactly`);
  console.log(`  once and that one reference is a callee, every invocation binds parameter i to the fold of`);
  console.log(`  that one argument — sound for the same reason a once-written slot is, and independent of how`);
  console.log(`  many times the call runs, because the argument is the same node each time.`);
  console.log(`  settled   ${String(tot.sole.settled).padStart(5)}   parameter(s) resolved to a COMPLETE text — a row this closure moved OUT of the razor`);
  console.log(`  unsettled ${String(tot.sole.unsettled).padStart(5)}   resolved to the caller's argument and it carries a hole too — the arm working, buying nothing`);
  for (const [k, v] of [...tot.sole.refused].sort((a, b) => b[1] - a[1]))
    console.log(`  refused   ${String(v).padStart(5)}   ${k}`);
  console.log(``);

  /* ── THE MUTABLE-FOLD FLOOR AND ITS PRICE ────────────────────────────────────────────────────────────────
     THE ONE NUMBER THAT MAKES A NAMED RESIDUAL A MEASUREMENT. `deref` demotes a `new URL` read out of a
     name, a slot or an object-literal property, because a URL is mutable and `searchParams.set` leaves no
     trace in the text this fold reads. Every demotion is therefore a row whose text was recovered COMPLETELY
     and is reported as a SHAPE, so this count is the exact CEILING on what an escape analysis over the URL's
     binding could ever recover — and the rows carry their coordinates, so whether each URL is really mutated
     is answered by READING THE SOURCE rather than by believing a sentence in the header. */
  /* ── WHY THE INCOMPLETE ROWS ARE INCOMPLETE ──────────────────────────────────────────────────────────────
     A PARTITION OF THE RAZOR'S OWN POPULATION, AND IT RESOLVES NOTHING BY CONSTRUCTION. `opaque` is correct
     and says only "a request happens here"; it cannot tell an address composed from a value ONLY A RUN HAS
     from one composed from a value a run has AND THE ENGINE ALREADY LEARNS. A promise resolution value, an
     array element and an Event are the second; a property nothing in the text writes is the first. So this
     block may not be priced by how many rows it moves out of `opaque` — it moves none — and its worth is
     that a reader can tell the two mechanisms apart per row rather than taking an aggregate on trust.
     THE TOTALS ARE ASSERTED TO SUM, which is the one check that says a partition has not become a second
     count of the same population: every hole carries a class, every incomplete row carries a hole, so the
     buckets must equal `shape + opaque` for their channel exactly. The classes are attached at the hole's
     ORIGIN and never inferred from what encloses the call — a door inside a `.then` callback whose address is
     `"/api/" + someGlobal` is not promise-derived, and a band keyed on the lexical surroundings would have
     said it was. */
  const bandSum = (m) => [...m.values()].reduce((a, b) => a + b, 0);
  for (const [k, want] of [["data", tot.data.shape + tot.data.opaque],
                           ["program", tot.program.shape + tot.program.opaque],
                           ["blind", tot.blind.shape + tot.blind.opaque]]) {
    const got = bandSum(tot.sig[k]);
    if (got !== want)
      die(`the WHY band's ${k} buckets sum to ${got} and that channel reports ${want} incomplete row(s). ` +
          `A partition whose parts do not sum to the population is a SECOND COUNT of it, so every figure ` +
          `it is read beside would be about a different question. Nothing further is printed.`);
  }
  console.log(``);
  console.log(`WHY THE INCOMPLETE ROWS ARE INCOMPLETE — the class of the value that would have finished each`);
  console.log(`  address, taken at the HOLE'S OWN ORIGIN rather than from what lexically encloses the call. It`);
  console.log(`  RESOLVES NOTHING and must not be priced by what it moves: it partitions the rows this file`);
  console.log(`  already reports as shape or opaque, and the buckets are ASSERTED to sum to that population.`);
  console.log(`  A row carrying holes of two kinds is listed under BOTH names joined, never ranked into one —`);
  console.log(`  a ranking is a convention nobody can check and a set is a fact about the row.`);
  console.log(`  WHAT THIS DOES NOT CLAIM: a promise resolution value is not a REPLY. The value \`p.then(f)\``);
  console.log(`  receives is whatever \`p\` settles with, which is a fact about \`p\`; calling it a reply needs the`);
  console.log(`  receiver traced to a data-door call, and that is a named residual and not a word chosen here.`);
  for (const [k, label] of [["data", `DATA door`], ["program", `PROGRAM door`], ["blind", `blind spot`]]) {
    const rows = [...tot.sig[k]].sort((a, b) => b[1] - a[1]);
    console.log(`  ${label} — ${bandSum(tot.sig[k])} incomplete row(s) in ${rows.length} class(es):`);
    for (const [sg, v] of rows) console.log(`    ${String(v).padStart(5)}   ${sg}`);
  }
  console.log(``);

  console.log(``);
  console.log(`THE MUTABLE-FOLD FLOOR AND WHAT IT COST — a constructed URL read through a NAME is demoted to a`);
  console.log(`  SHAPE, because a URL is mutable and a searchParams write leaves nothing in the text. Each row`);
  console.log(`  below is one whose text this fold recovered COMPLETELY and which is reported as incomplete, so`);
  console.log(`  the count is the CEILING on what proving the absence of mutation could recover — and the`);
  console.log(`  coordinates are printed so that question is settled by reading the bundle, never by a claim here.`);
  console.log(`  demoted ${tot.mutFloor.demoted}`);
  for (const row of tot.mutFloor.rows.slice(0, 12))
    console.log(`    [${row.chan}] ${row.file}:${row.line}  ${row.url.slice(0, 120)}`);
  if (tot.mutFloor.rows.length > 12) console.log(`    ... and ${tot.mutFloor.rows.length - 12} more`);
  console.log(``);

  console.log(``);
  console.log(`AND WHETHER A CALL-RESULT HOLE IS IN THE BUNDLE'S TEXT AT ALL — the SECOND FIELD on the band's`);
  console.log(`  largest class, and its UNIT IS A CALL EXPRESSION rather than a door row or an address, so it`);
  console.log(`  is not subtractable from either: one row can carry several call holes and one call can be a`);
  console.log(`  hole in several rows. \`a call result\` IS NOT EVERY CALL: the door channel's inline arm crosses`);
  console.log(`  the callee boundary already, and where it succeeds the hole is attributed INSIDE the callee, so the`);
  console.log(`  class is EXACTLY the population that arm REFUSED. This asks the question a refusal reason cannot —`);
  console.log(`  does the callee resolve to ONE definition here whose return folds to text from this file alone. It`);
  console.log(`  RESOLVES NOTHING and moves no total; the band's buckets are byte-identical beside it.`);
  console.log(`  WHICH WAY IT ERRS: by admitting too much. A speculatively-discarded fold is still counted and`);
  console.log(`  a definition is credited from its returns rather than from a path a run would take, so this`);
  console.log(`  OVERSTATES what a parse could reach — the conservative direction here, because understating`);
  console.log(`  the baseline is what would flatter the engine.`);
  {
    const rows = [...tot.callResult.verdict].sort((a, b) => b[1] - a[1]);
    const total = rows.reduce((a, b) => a + b[1], 0);
    console.log(`  ${total} distinct call expression(s) whose result the fold made a hole, in ${rows.length} verdict(s):`);
    for (const [v, n] of rows) console.log(`    ${String(n).padStart(5)}   ${v}`);
    /* THE CREDITED COUNT IS A SCORED ZERO AND NOT A SILENT ONE. The selftest carries a control for this
       verdict — a two-statement body returning a literal, which is a shape the inline arm refuses by its own
       stated reason — and DIES if it never fires, so a 0 here is a fact about the corpus rather than a
       classifier that stopped classifying. */
    const credited = tot.callResult.rows.filter((r) => r.v === CALLEE_IN_FILE);
    const resolved = tot.callResult.rows.filter((r) => r.v === CALLEE_NOT_TEXT);
    console.log(`  OF THOSE, ${credited.length + resolved.length} had their callee resolved to ONE definition in the same file,`);
    console.log(`  and ${credited.length} of them return text this file can fold. Both figures are shown firing by a control.`);
    console.log(`  AND THE ROWS A READER CAN CHECK — a claim about the bundle, settled by opening the coordinate.`);
    for (const row of credited.slice(0, 12))
      console.log(`    [in the text]  ${row.file}:${row.line}  ${row.callee}() -> ${String(row.text).slice(0, 90)}`);
    if (credited.length > 12) console.log(`    ... and ${credited.length - 12} more in the text`);
    const dep = resolved.filter((r) => r.param), own = resolved.filter((r) => !r.param);
    console.log(`  AND THE RESOLVED ONES SPLIT ON WHETHER THE RETURN EXPRESSION NAMES A PARAMETER: ${dep.length} do and`);
    console.log(`  ${own.length} do not. The first is the bundler chunk-function shape this file's MANIFEST channel already`);
    console.log(`  enumerates, so that address is in the text per candidate under another unit. THE SECOND IS NOT`);
    console.log(`  CALLER-INDEPENDENT and this block used to say it was: three of its rows opened in the mirror each`);
    console.log(`  return a LOCAL that is an alias of a parameter one write earlier, so both sides reduce to ONE next`);
    console.log(`  diff — a fold over the callee's own straight-line writes — and the bit says which side needs it.`);
    console.log(`  It is resolved through the scope pass rather than by name, and UNDER-counts parameter dependence.`);
    for (const row of dep.slice(0, 8))
      console.log(`    [resolved, return uses a parameter]  ${row.file}:${row.line}  ${row.callee}()`);
    if (dep.length > 8) console.log(`    ... and ${dep.length - 8} more using a parameter`);
    for (const row of own.slice(0, 8))
      console.log(`    [resolved, return uses no parameter]  ${row.file}:${row.line}  ${row.callee}()`);
    if (own.length > 8) console.log(`    ... and ${own.length - 8} more using no parameter`);
  }
  console.log(``);


  console.log(``);
  console.log(`HOW A REAL BUNDLE SPELLS THE NAMES THE ENGINE'S COMPILER-SIDE ROWS COUNT — the population a`);
  console.log(`  MEMBER-NAME CHANNEL would add, measured before it is built. Two engine components raise a row`);
  console.log(`  when the compiler resolves a FREE IDENTIFIER against the global object, and both record the same`);
  console.log(`  residual: the row sees that ONE spelling, so a name reached as a property of the global object`);
  console.log(`  raises nothing. Those rows are read as a BIT, so the floor costs a READING only at a site that`);
  console.log(`  spells a name ONLY as a property — which is the column to read and is the last one here.`);
  console.log(`  NOT sites, NOT endpoints, summed into no door total: these are NAME OCCURRENCES in the text.`);
  console.log(`  names derived from ${ENTRY_DECL.size} engine declaration(s), never typed here:`);
  for (const nm of [...ENTRY_NAMES].sort())
    console.log(`    ${nm.padEnd(22)} ${[...new Set(ENTRY_DECL.get(nm))].join(" ")}`);
  console.log(``);
  console.log(`  occurrences by spelling            what the landed row sees |  what a member channel would add  | wrapper`);
  console.log(`  name                     bareFree boundName typeof | qualif boundGl compLit destr | instMem instComp`);
  for (const nm of [...ENTRY_NAMES].sort()) {
    const t = tot.spell[nm] || spellTally();
    console.log(`  ${nm.padEnd(22)} ${String(t.bareFree).padStart(8)} ${String(t.bareBoundName).padStart(9)} ` +
                `${String(t.typeofBare).padStart(6)} | ${String(t.qualified).padStart(6)} ` +
                `${String(t.qualifiedBoundGlobal).padStart(7)} ${String(t.computedLiteral).padStart(7)} ` +
                `${String(t.destructuredFromGlobal).padStart(5)} | ${String(t.instanceMember).padStart(7)} ` +
                `${String(t.instanceMemberComputed).padStart(8)}`);
  }
  console.log(`  a bare reference in a file that also BINDS the name is boundName and is counted apart: the`);
  console.log(`  engine's resolver is scope-correct and very probably DOES see it, and this file-wide pass`);
  console.log(`  cannot prove which, so neither answer below is allowed to assume it. typeof is NOT a`);
  console.log(`  partition member and is not summed — every one of those is already inside one of the two bare`);
  console.log(`  columns, and it says the name was PROBED for rather than used. An alias is not a column at all:`);
  console.log(`  \`const f = fetch\` and \`const f = window.fetch\` are already counted at the spelling each used.`);
  console.log(``);
  console.log(`  AND THE SAME THING PER SITE, WHICH IS THE UNIT THE LANDED ROW IS ZERO AT. \`any\` is the sites`);
  console.log(`  whose text reaches the platform name by ANY global spelling; a wrapper's own member is excluded`);
  console.log(`  from it, because no global-resolution channel of any spelling would raise one.`);
  console.log(`  name                     sites:any  bareFree  bareAny  propAny  PROP-ONLY(tight)  (loose)  wrapperOnly`);
  for (const nm of [...ENTRY_NAMES].sort()) {
    const s = tot.spellSites[nm] || { any: 0, bareFree: 0, bareAny: 0, propertyAny: 0, propertyOnlyTight: 0, propertyOnlyLoose: 0, wrapperOnly: 0 };
    console.log(`  ${nm.padEnd(22)} ${String(s.any).padStart(9)} ${String(s.bareFree).padStart(9)} ` +
                `${String(s.bareAny).padStart(8)} ${String(s.propertyAny).padStart(8)} ` +
                `${String(s.propertyOnlyTight).padStart(17)} ${String(s.propertyOnlyLoose).padStart(8)} ` +
                `${String(s.wrapperOnly).padStart(12)}`);
  }
  console.log(`  PROP-ONLY(tight) is the population the member channel RESCUES: a site whose text spells the`);
  console.log(`  name as a property of the global object and never as a bare identifier at all, so the landed`);
  console.log(`  row reads 0 about a program that does name the entry. (loose) counts a site whose only bare`);
  console.log(`  reference sits in a file that binds the name, and is an OVER-count of the same thing — the two`);
  console.log(`  bracket it, and the spread between them is what this pass cannot decide without a scope graph.`);
  console.log(`  ${tot.spellOther.globalComputedDynamic} further read(s) of a COMPUTED member of the global object`);
  console.log(`  belong to no name at all (\`self[n]\`) and are attributed to none: a channel keyed on a property`);
  console.log(`  NAME cannot recover one either, so this is a floor under BOTH columns and not under one.`);
  {
    /* AND WHAT SUCH A CHANNEL WOULD COST, WHICH IS THE HALF A RECALL FIGURE ALONE WOULD NOT PRICE. A field-get
       emitter sees the PROPERTY NAME and, unless it also tests the receiver, cannot tell the global object's
       own member from a wrapper's or a bundler's re-export shim — and the two are not close in this corpus.
       THE ENGINE CAN MAKE THAT TEST AND THIS PASS CANNOT, which is the one place the comparison runs the other
       way: at a field get the engine holds the receiver OBJECT and can ask whether it is the realm's global,
       while a parse has only a name it must refuse to guess about. So this number is not an argument against
       the channel; it is the size of the thing the channel has to get right to be a refinement of the row
       rather than a louder and different one. */
    let g = 0, w = 0;
    for (const nm of ENTRY_NAMES) {
      const t = tot.spell[nm]; if (!t) continue;
      g += t.qualified + t.qualifiedBoundGlobal + t.computedLiteral + t.destructuredFromGlobal;
      w += t.instanceMember + t.instanceMemberComputed;
    }
    console.log(`  AND ITS PRICE: of ${g + w} property read(s) of a declared name, ${g} are on the global object`);
    console.log(`  and ${w} are on a receiver that is not it. A field-get channel that does not TEST the receiver`);
    console.log(`  raises both, so it would not refine the landed row — it would be a different and louder one.`);
    console.log(`  The engine can make that test where this pass cannot: at a field get it holds the receiver`);
    console.log(`  OBJECT and can ask whether it is the realm's global, and a parse holds only a name.`);
  }
  console.log(``);
  console.log(`THE CHUNK MANIFEST, RECOVERED — addresses a bundler emits as a MAP plus a public-path literal,`);
  console.log(`  composed in a one-parameter function and handed to an injected <script>. Every one is plain`);
  console.log(`  literal text in the file, so a PARSE has them; the door channel recovers none, because the`);
  console.log(`  composition crosses a function boundary. NOT request sites, summed into NO door total, and`);
  console.log(`  never quotable as endpoints: a manifest names every chunk the bundle COULD load and one run`);
  console.log(`  loads a few. This is the number the PROGRAM-door zeros in the table below are explained by.`);
  console.log(`  compositions ${tot.manifest.sites}, of which ${tot.manifest.addressSites} recovered an ADDRESS   ` +
              `distinct addresses ${tot.manifestDistinctUrls}   ${tot.manifest.multi} enumerate more than one`);
  console.log(`  ${tot.manifest.candidates} candidate key(s) were drawn from the functions' own bodies; ` +
              `${tot.manifest.dropped} dropped for a hole and`);
  console.log(`  ${tot.manifest.fragments} folded to a FRAGMENT — a chunk name with no public path in front of`);
  console.log(`  it, which is part of an address and is refused rather than emitted as a whole. This is also`);
  console.log(`  the precision half: without the shape test the channel called \`session\`, \`Users\` and`);
  console.log(`  \`0.001\` addresses, because indexing a string table is an i18n or enum lookup as often as`);
  console.log(`  it is a chunk manifest and nothing about the code's SHAPE tells the two apart.`);
  console.log(`  ${tot.manifest.refusedTwoApplications} composition(s) were REFUSED for holding two ` +
              `applications: two unknown parameters make the`);
  console.log(`  address set a product of two domains, and nothing here has established the two are indexed together.`);
  console.log(``);
  console.log(`BOTH CLASSES ${tot.sites} sites, ${tot.distinctUrls} distinct addresses — printed last and`);
  console.log(`  never first, because ${pct(tot.program.sites, tot.sites)} of it is chunk loading.`);
  console.log(`  literal ${tot.literal}  folded ${tot.folded}  shape ${tot.shape}  opaque ${tot.opaque}  guarded ${tot.guarded}`);
  console.log(`  branchAlt ${tot.branchAlt}  conditional URLs where the text carries BOTH arms and one run takes one`);
  console.log(`  doors the engine's endpoint_record records: ${tot.engineDoorSites}; doors it does not: ${tot.nonEngineDoorSites}`);
  console.log(`  by door: ${Object.entries(tot.byDoor).sort((a, b) => b[1] - a[1]).map(([k, v]) => `${k}=${v}`).join("  ") || "(none)"}`);
  console.log(``);
  /* THE DENOMINATOR IS THE POPULATION THE COLUMNS BELOW ARE DRAWN FROM AND IT IS ASSERTED TO BE, because a
     banner naming one number over rows tallied from another is the defect CLAUDE.md
     §a-coverage-figure-states-what-it-is-a-fraction-of is about. It used to be `sites - literal`, which
     counted the FOLDED rows the fold itself settled. */
  const unsettled = tot.data.shape + tot.data.opaque;
  { const inShapes = Object.values(tot.argShape).reduce((a, b) => a + b, 0);
    if (inShapes !== unsettled)
      die(`the stronger-parser band names ${unsettled} unsettled DATA-door site(s) and its argument-shape ` +
          `rows tally ${inShapes}. One of the two is drawn from a different population than the banner ` +
          `states, and every percentage under it would be a fraction of something unstated.`); }
  const idents = Object.values(tot.bindBuckets).reduce((a, x) => a + x, 0);
  console.log(`COULD A STRONGER PARSER HAVE DONE BETTER? — asked of the ${unsettled} DATA-door site(s) whose`);
  console.log(`  URL this file's fold did NOT settle. Its fold is conservative ON PURPOSE, so its opaque`);
  console.log(`  count is a FLOOR for the parse; these two rows bound how much of a floor.`);
  console.log(`  argument shape: ${Object.entries(tot.argShape).sort((a, b) => b[1] - a[1]).map(([k, v]) => `${k}=${v}`).join("  ")}`);
  console.log(`  of the ${idents} whose URL is a bare NAME, times that name is bound in its own FILE — which`);
  console.log(`  is a fact about what the MINIFIER did and not about what a resolver can reach:`);
  for (const [k, v] of Object.entries(tot.bindBuckets))
    if (v) console.log(`    ${String(k).padStart(7)} : ${String(v).padStart(4)}  ${k === "1" ? "<- any parser resolves this; THIS FILE ALREADY DOES" : k === "101+" ? "<- a register reused across scopes; THIS FILE RESOLVES THESE TOO WHERE ONE SCOPE SETTLES THEM" : ""}`);
  /* THE 101+ ROW USED TO READ `a minifier's reused register; no name-based resolution can touch it`, AND THE
     DIFF THAT MADE THE FOLD SCOPE-CORRECT REFUTED IT. That annotation is kept here in its own words because
     it is the reading a reader re-derives from the column: a bare `i` bound a hundred times in a bundle looks
     unreachable to any resolver. It is not — a name bound once IN ITS OWN SCOPE is settled whatever the
     file-wide count says, and this corpus's replit rows are four real API paths (`/api/v1/auth/sign-in` and
     its siblings) held in a `let i` inside one arrow function, in files that bind `i` in the hundreds. So a
     high count in this column is evidence about the MINIFIER and never about a ceiling, and the rows that
     remain here are the ones no resolution settled rather than the ones no resolution could. */
  console.log(`  ${tot.oneCharNames} of ${idents} of those names are ONE CHARACTER long.`);
  console.log(``);
  console.log(`BASE RATE (not endpoints, never quote as such): ${tot.pathish} distinct address-shaped string`);
  console.log(`  literals in the same programs that are attached to NO door. A naive extractor reports these;`);
  console.log(`  this one does not. A zero above would have to be read against this number.`);
  console.log(``);
  console.log(``);
  console.log(`PER SITE — the DATA door only; the program door is in --json. The last three columns are the`);
  console.log(`  REACH partition of \`data\` (top-level / inside an async fn / inside a sync fn), printed here`);
  console.log(`  because per-site is where it answers: a site's data-door count and how much of it needs an`);
  console.log(`  invoker are one reading and two numbers.`);
  console.log(`site             programs   data  literal folded  shape opaque guarded   urls  pathish   top  async   sync`);
  for (const s of out.perSite)
    console.log(`  ${s.site.padEnd(14)} ${String(s.programs).padStart(8)} ${String(s.data.sites).padStart(6)} ` +
                `${String(s.data.literal).padStart(8)} ${String(s.data.folded).padStart(6)} ${String(s.data.shape).padStart(6)} ` +
                `${String(s.data.opaque).padStart(6)} ${String(s.data.guarded).padStart(7)} ` +
                `${String(s.data.distinctUrls).padStart(6)} ${String(s.pathish).padStart(8)} ` +
                `${String(s.data.topLevel).padStart(5)} ${String(s.data.innerAsync).padStart(6)} ` +
                `${String(s.data.innerSync).padStart(6)}`);
  console.log(``);
  /* PER SITE, THE PROGRAM DOOR BESIDE THE BLIND SPOT, WHICH IS THE ONLY PLACE THE TWO CAN BE READ TOGETHER.
     A site whose program door reads 0 has either shipped no chunk loader or loaded its chunks through the
     column to its right, and a table printing one without the other cannot tell those apart. MEASURED and
     the reason this block exists: the program door is BIMODAL BY BUNDLER — a bundle that emits native
     `import()` scores in the door, and one whose bundler compiled `import()` away into a chunk-id map plus
     a `<script>` injection scores ZERO there and scores here instead. Neither zero is a statement about how
     much the site loads. */
  console.log(`PER SITE — the PROGRAM door against the blind spot it can be lost in, and the manifest it was`);
  console.log(`  lost INTO. A program-door 0 beside a nonzero b.opaq used to be the whole reading; m.urls is`);
  console.log(`  the addresses those opaque assignments were carrying, so the three columns answer together.`);
  console.log(`site             programs  prog  p.lit p.fold p.urls | blind  b.lit b.fold b.shape b.opaq  b.urls | m.sites m.urls`);
  for (const s of [...out.perSite].sort((a, b) => b.blind.sites - a.blind.sites || b.program.sites - a.program.sites))
    console.log(`  ${s.site.padEnd(14)} ${String(s.programs).padStart(8)} ${String(s.program.sites).padStart(5)} ` +
                `${String(s.program.literal).padStart(6)} ${String(s.program.folded).padStart(6)} ` +
                `${String(s.program.distinctUrls).padStart(6)} | ${String(s.blind.sites).padStart(5)} ` +
                `${String(s.blind.literal).padStart(6)} ${String(s.blind.folded).padStart(6)} ` +
                `${String(s.blind.shape).padStart(7)} ${String(s.blind.opaque).padStart(6)} ` +
                `${String(s.blind.distinctUrls).padStart(7)} | ${String(s.manifest.sites).padStart(7)} ` +
                `${String(s.manifest.distinctUrls).padStart(6)}`);
  if (parseFail.length) { console.log(``); console.log(`PARSE REFUSED (${parseFail.length}):`); for (const p of parseFail.slice(0, 10)) console.log(`  ${p}`); }
  if (nExamples) {
    console.log(``); console.log(`EXAMPLES:`);
    for (const r of out.examples) console.log(`  [${r.kind}/${r.door}/g${r.guard}] ${r.method} ${r.url.slice(0, 120)}${r.alt ? `   (alt ${r.alt.slice(0, 60)})` : ""}   ${r.file}:${r.line}`);
    console.log(``); console.log(`MANIFEST EXAMPLES (one composition, its first addresses):`);
    for (const m of out.manifestExamples) {
      console.log(`  ${m.file}:${m.line}  ${m.candidates} candidate(s), ${m.dropped} dropped, ${m.fragments} fragment(s)`);
      for (const a of m.addresses) console.log(`      ${a.slice(0, 140)}`);
    }
  }
}

main(process.argv.slice(2));
