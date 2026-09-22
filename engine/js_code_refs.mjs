/* WHICH OCCURRENCES OF A PLATFORM NAME A PROGRAM ACTUALLY EVALUATES — TAKEN FROM A REAL PARSE OF THAT PROGRAM.
 *
 * engine/absentrank.mjs ranks an absence by matching identifiers in the raw bytes of real bundles, and raw
 * bytes carry more than code: a codegen template, a plugin shipped as a string, a debug line naming an API.
 * Its own NAMED RESIDUAL says so and names what to build — "a real JS tokenizer producing a code/not-code
 * mask" — and records that a HAND-ROLLED one was written, measured and DELIBERATELY NOT LANDED, because it
 * mis-masked a span of one real bundle and removed a genuine `new FontFace(...)` and a genuine
 * `e instanceof ImageData`. An exclusion wrong in the REMOVING direction is an under-claim, and an
 * under-claim is not found by acting on it. So the failure to avoid is not "no mask"; it is a mask that
 * silently retires true rows out of the band its caller sorts FIRST.
 *
 * THE MASK IS THEREFORE NOT WRITTEN HERE AT ALL. CLAUDE.md §Bind-before-build orders the sources — host
 * runtime, engine intrinsic, existing module, faithful port, hand-roll LAST — and the regex/division
 * ambiguity that defeated the hand-rolled attempt is not decidable by a lexer: it needs the parser context
 * ECMAScript §12.10 "Automatic Semicolon Insertion" and §12.9.5 "Regular Expression Literals" leave to the
 * goal symbol. esbuild is already a devDependency of this project, is a real JS parser, and will re-print a
 * program from its own parse. This file asks IT the question and reads its answer.
 *
 * WHAT IS ASKED IS A RENAME, NOT A SPAN, AND THAT IS THE LOAD-BEARING CHOICE. A span mask answers "is this
 * offset inside a literal" and still counts a bundle's own local named after an interface. esbuild's
 * `define` substitutes a name only where it is an EVALUATED FREE REFERENCE — never inside a string, a
 * template, a regex or a comment, and never where an enclosing SCOPE binds it. A MARK IS THEREFORE `CODE`
 * AND NOT `A READ`, and the difference is measured rather than conceded: `{X: 1}`, `class C { X(){} }` and
 * `window.X = 1` are all MARKED, and a property definition and a member write are not reads. That is the
 * right answer for the question a caller is asking — is this occurrence something the program EVALUATES, or
 * text a page never runs — and it is the wrong word for it, so the word is not used. Saying `a read` would
 * be an over-claim one object literal refutes, and a caller that refuted it would discard the true part with
 * the false. The question is answered by
 * the parser's own binding resolution rather than by a rule restated here, which is the same discipline
 * engine/idlgen.mjs follows in reading the real `.idl` and engine/corpus_programs.mjs in reading the
 * server's own Content-Type.
 *
 * WHAT IT COVERS IS MEASURED PER OPERAND POSITION AND NOT ASSUMED, because two different esbuild mechanisms
 * answer two different halves and they do not have the same reach. Verified by exercising the transform
 * (engine/js_code_refs.mjs's own arming below runs every one of these on every construction):
 *   - an IDENTIFIER read (`new X(`, `x instanceof X`, `typeof X`, `X.member`'s receiver, `f(a,X)`) is
 *     `define`'s population. Exact, and SCOPE-CORRECT: a binding in an inner function does not stop an outer
 *     reference being marked, and a binding in the enclosing scope does.
 *   - a DOT PROPERTY (`window.X`, `self.X`, `globalThis.X`, and the optional-chained `a?.X`) is
 *     `mangleProps`'s population. Exact.
 *   - a QUOTED KEY (`window["X"]`, `"X" in window`) is `mangleQuoted`'s population AND IS COVERED ONLY FOR
 *     THE `"` AND `'` DELIMITERS. A TEMPLATE LITERAL IS NOT A QUOTED PROPERTY TO ESBUILD: measured by
 *     exercising it, `"X" in window` and `'X' in window` are renamed and ``​`X` in window`` is NOT, and the
 *     same split holds for `window["X"]` against ``window[`X`]``. That is not an edge case in this corpus —
 *     absentrank's own header records bundles here emitted with EVERY string spelled with a backtick — so a
 *     caller that read an unrenamed backtick key as "not code" would retire GENUINE feature detections.
 *     MEASURED over a 715-program corpus before this was understood: twelve such guards, on `ontouchstart`,
 *     `AnimationEvent`, `TransitionEvent`, `CompositionEvent`, `TextEvent`, `performance` and
 *     `Notification`, every one of them real. The caller is told which positions are judgeable and the
 *     backtick form is a FLOOR rather than a silent retirement.
 *     THAT COUNT IS WHAT THE DECISION COST AND NOT WHAT THE POPULATION IS, which is the only reason it
 *     stays: it is why this position is floored rather than retired, and it would be worth recording had it
 *     been one guard. It is also NOT re-derivable from a checkout — the corpus is other people's fetched
 *     bytes, which this tree deliberately does not carry — so it is a fact about one instant, and what is
 *     handed over is the drive rather than the figure:
 *       NODE_USE_ENV_PROXY=1 SITES=apps.tsv node testing/corpus/fetch.mjs   # prints the --corpus path
 *     The SPLIT ITSELF needs none of that and is re-derived in one command, which is what the arming below
 *     does on every construction and what a reader should trust instead of the count.
 *
 * THE THREE GLOBAL RECEIVER SPELLINGS ARE EXCLUDED FROM THE RENAME AND THE REASON IS A MEASUREMENT.
 * `window` and `self` are themselves members of Window and so are in the platform's own name table, so
 * defining them rewrites `window.performance` to `window<mark>.performance<mark>` and every receiver-anchored
 * pattern anchored on the literal word `window` stops matching. Measured before they were excluded: the
 * `window.X` and `self.X` channels reported ZERO for `performance` where the corpus reads it 92 times. They
 * are the three receivers absentrank already treats as the global, so excluding them costs no new
 * assumption — and a caller that RANKS one of them is told, by name, that this reader cannot speak about it.
 *
 * WHAT IT DOES NOT COVER, stated because a caller subtracting this from a count needs to know what the count
 * still holds:
 *   - A COMMENT-borne occurrence is not retired. esbuild DROPS comments, so such an occurrence is absent
 *     from the re-printed text entirely: it is neither marked nor left bare, and a caller counting the bare
 *     ones never sees it. Retiring it needs a count taken over the re-print rather than over the raw bytes,
 *     which is a second reading of the corpus and a separate diff.
 *   - A SCOPE-BOUND occurrence reads one of TWO ways and only one of them is invisible, which is measured
 *     because the difference decides how blind a caller is told this reader is. With NO colliding free
 *     reference in the same file — the ordinary case, a bundle's own local — the binding keeps its spelling
 *     and reads BARE, which is the answer a caller wants and is this reader working. Only where the file
 *     ALSO holds a genuine free reference does esbuild RENAME the local out of the way (measured:
 *     `new X(1); function g(){var X=1;return X}` re-prints the local as `X2`), and that occurrence is then
 *     neither marked nor bare. Both are safe — a vanished occurrence inflates no count — and an earlier
 *     draft of this sentence claimed the second case for both, which understates the reader in the silent
 *     direction. An IMPORTED binding is the same shape and also reads bare: `import {X} from "m"; new X(1)`
 *     marks nothing, so a bundle's own module binding is not read as the platform's. absentrank's per-file
 *     binder column remains what covers the renamed-away case, and this reader is disjoint from it.
 *   - A file the parser REFUSES is reported as unparsed and nothing is claimed about it. The MECHANISM is
 *     re-derivable with no corpus and was: a document opening `<html>` refuses at 1:0, so the residual's own
 *     "`.html` left whole because its markup is not JS" falls out of the parser rather than out of a
 *     filename, which is what engine/corpus_programs.mjs requires (it takes the population from the server's
 *     recorded Content-Type). The COUNTS that stood here — 715 parsed, 0 refused, 19 documents refusing —
 *     are a fact about one fetched instant of other people's sites and cannot be re-derived from a checkout,
 *     so they are not restated: a caller wanting today's figures runs the corpus drive named above and reads
 *     this reader's own `parsed:false` tally, which is the derivation and does not rot.
 *
 * IT IS ARMED POSITIVE AND NEGATIVE ON EVERY CONSTRUCTION AND THROWS RATHER THAN RETURNING A QUIET ZERO,
 * because a reader whose marking silently stopped working would report every occurrence as code — which is
 * the direction that leaves the caller's counts exactly as they were and looks like a mask that found
 * nothing. The negatives are the shapes this exists to refuse: the same text inside a string, inside a
 * template, inside a comment, and bound by the enclosing scope. */
import * as esbuild from "esbuild";

/* THE MARKER IS A SUFFIX SO THAT A MARKED NAME IS STILL AN IDENTIFIER OF THE SAME SHAPE — a caller's channel
   patterns capture `[A-Za-z_$][\w$]*` and `[A-Z][\w$]*`, and a suffix keeps both true of `Foo<mark>` while a
   prefix would not. It is asserted ABSENT from every source read, so a bundle that already spells it cannot
   be read as a mark this file placed. */
export const MARK = "$AbsentRankRef$";

/* The three receivers a caller anchors on literally. Excluded from the rename; see the header. */
export const GLOBAL_RECEIVERS = new Set(["window", "self", "globalThis"]);

/* A name that must never be read. Defined like every other, so a zero from it is a MEASURED zero rather than
   a question nobody asked — the caller runs it over the same corpus in the same pass as its subject. */
export const CONTROL_NAME = "AbsentRankNeverRead";

/* ECMAScript §12.7.2 "Keywords and Reserved Words" plus the two §13.15.1 early-error names. A reserved word
   cannot be a define key, and the platform's table holds none today — this is a guard against a table that
   grows one, so the failure is a THROW naming it rather than an esbuild error nobody can place. */
const RESERVED = new Set(["await", "break", "case", "catch", "class", "const", "continue", "debugger",
  "default", "delete", "do", "else", "enum", "export", "extends", "false", "finally", "for", "function",
  "if", "import", "in", "instanceof", "let", "new", "null", "return", "static", "super", "switch", "this",
  "throw", "true", "try", "typeof", "var", "void", "while", "with", "yield", "eval", "arguments"]);

const esc = (n) => n.replace(/[$]/g, "\\$");

/* A caller asks whether a BARE occurrence it found in the re-printed text is evidence that the occurrence is
   not code. For every operand position but one it is; for a quoted key it is evidence only when the key is
   delimited by `"` or `'`, because a template literal is outside `mangleQuoted`'s reach. The caller passes
   the matched text, which carries its own delimiter. */
export const quotedKeyJudgeable = (matchText) => !matchText.includes("\x60");

/* A CALLER ALSO NEEDS TO KNOW WHICH OF TWO STATES A *BARE* OCCURRENCE IS IN, AND THE RENAME ABOVE CANNOT SAY.
   `define` substitutes a FREE reference, so an occurrence it leaves bare is one of two things that take
   opposite work: text a page never runs, or a reference to a binding THE FILE ITSELF MAKES. Both are equally
   not-the-platform and a caller subtracting them needs no split; a caller PRINTING them does, because
   "source carried as data" sends a reader to open the sites and "the page's own name" tells them not to.
   The question is answered by the same binding resolution and NOT by a second spelling rule: a reference
   APPENDED to the program is marked IFF nothing in scope at that point binds the name. The sentinel is a
   string so the tail can be located in the re-print without matching on the caller's own names.
   WHAT IT COVERS IS THE SCOPE THE APPENDED REFERENCE STANDS IN, WHICH IS THE TOP-LEVEL ONE, and the arming
   below asserts both halves of that: every top-level binding form answers BOUND — `var X;` with no
   initialiser, `X ||= {}`, a destructure, an import, `let`/`const`/`function`/`class` — and a string, a
   template, a comment, a free read and a binding in an INNER scope all answer FREE. The inner-scope answer
   is correct for what this entry claims (nothing at the end of the program binds the name) and is NOT the
   caller's whole question, which is why the caller keeps its own binder as the floor for that one case.
   THE PROBE PERTURBS THE PROGRAM AND SO IS ITS OWN TRANSFORM, NEVER THE ONE THE CALLER COUNTS FROM: adding
   a free reference is exactly what makes esbuild rename a colliding inner binding out of the way (measured
   in the header above), so an occurrence count taken from this re-print would be smaller than the truth —
   the REMOVING direction. Read the tail; discard the body. */
export const BIND_SENTINEL = "AbsentRankBindProbe";
/* The appended statement, and the bounded test for one marked name in its re-print. `endsWith`/`includes`
   would be wrong here: `Text` and `TextEvent` are both platform names, and an unbounded test for the first
   would read the second's mark as its own. */
const bindProbe = (names) => `\n;[${JSON.stringify(BIND_SENTINEL)},${names.join(",")}];\n`;
const markedIn = (text, n) => new RegExp(`(?<![\\w$])${esc(n)}${esc(MARK)}(?![\\w$])`).test(text);

export async function referenceReader(names) {
  const die = (s) => { throw new Error(`[js_code_refs] ${s}`); };
  const bad = names.filter((n) => !/^[A-Za-z_$][\w$]*$/.test(n));
  if (bad.length)
    die(`${bad.length} name(s) are not identifiers and cannot be a define key: ` +
        `${bad.slice(0, 6).map((n) => JSON.stringify(n)).join(", ")}. A name table that holds one has ` +
        `changed shape; read it before widening this filter, because dropping them silently would read a ` +
        `smaller absence as progress.`);
  const res = names.filter((n) => RESERVED.has(n));
  if (res.length)
    die(`${res.length} name(s) are ECMAScript §12.7.2 reserved words and cannot be renamed: ` +
        `${res.join(", ")}. A caller ranking one of these must be told this reader cannot speak about it, ` +
        `never handed a zero.`);

  /* The names this reader will not speak about, returned so a caller can say so rather than print a zero. */
  const excluded = names.filter((n) => GLOBAL_RECEIVERS.has(n));
  const mark = names.filter((n) => !GLOBAL_RECEIVERS.has(n));
  /* ASKED OF THE CALLER'S NAMES, BECAUSE `use` BELOW ALWAYS HOLDS THE CONTROL AND SO CAN NEVER BE EMPTY. The
     condition this replaces was `!use.length`, whose two sides cannot disagree: measured, `referenceReader([])`
     and `referenceReader(["window","self","globalThis"])` both returned a reader rather than dying. That is a
     non-check wearing the syntax of a check, and the state it certifies is the removing one — a reader that
     marks NONE of the caller's names, arms itself green on its own control, and reports every occurrence of
     them bare, which retires the whole table. */
  if (!mark.length)
    die((names.length ? `all ${names.length} name(s) passed are global receivers this reader excludes`
                      : `the caller passed no names at all`) +
        `, so it would mark none of them and a caller would read every occurrence as bare — retiring rows ` +
        `rather than ranking them. A caller ranking only these must be told this reader cannot speak about ` +
        `them, never handed a zero.`);
  const use = mark.concat([CONTROL_NAME]);

  const define = Object.fromEntries(use.map((n) => [n, n + MARK]));
  const seed = Object.fromEntries(use.map((n) => [n, n + MARK]));
  const mangleProps = new RegExp("^(" + use.map(esc).join("|") + ")$");
  const opts = { loader: "js", minify: false, legalComments: "none", treeShaking: false,
                 define, mangleProps, mangleQuoted: true, logLevel: "silent" };

  const transform = async (src) => (await esbuild.transform(src, { ...opts, mangleCache: { ...seed } })).code;

  /* ---- armed before a single corpus byte is read ------------------------------------------------------ */
  const probe = use.includes("FontFace") ? "FontFace" : use[0];
  const P = probe + MARK;
  const armed = await transform(
    `new ${probe}(1);\n` +                                   /* identifier: constructed        */
    `x instanceof ${probe};\n` +                             /* identifier: instanceof right   */
    `typeof ${probe};\n` +                                   /* identifier: typeof operand     */
    `${probe}.someMember;\n` +                               /* identifier: member receiver    */
    `Ue(a, ${probe});\n` +                                   /* identifier: call argument      */
    `window.${probe}; self?.${probe};\n` +                   /* dot property, optional chain   */
    `window["${probe}"]; "${probe}" in window;\n` +          /* quoted key, both shapes        */
    `/* comment: new ${probe}(2) */\n` +                     /* NEGATIVE: comment              */
    `const s = "new ${probe}(3)";\n` +                       /* NEGATIVE: string               */
    `const t = \`new ${probe}(4)\`;\n` +                     /* NEGATIVE: template             */
    `const r = /new ${probe}\\(5\\)/;\n` +                   /* NEGATIVE: regex body           */
    `function g() { var ${probe} = 6; return ${probe}.x }\n`);/* NEGATIVE: bound in scope      */
  for (const form of [`new ${P}(`, `instanceof ${P}`, `typeof ${P}`, `${P}.someMember`, `, ${P})`,
                      `window.${P}`, `self?.${P}`, `window.${P}`, `"${P}" in window`])
    if (!armed.includes(form))
      die(`the reader did not mark ${JSON.stringify(form)} in its own positive control. Its zero over a ` +
          `corpus would mean nothing, and a caller subtracting it would subtract nothing while reading it ` +
          `as a mask that found no text. esbuild's define/mangleProps behaviour has changed — read the ` +
          `transform's output before widening anything.\n--- output ---\n${armed}`);
  for (const form of [`new ${probe}(3)`, `new ${probe}(4)`, `new ${probe}\\(5\\)`])
    if (!armed.includes(form))
      die(`the reader's negative control ${JSON.stringify(form)} is not present UNMARKED in the re-printed ` +
          `text, so this cannot be shown to leave literal text alone.\n--- output ---\n${armed}`);
  if (armed.includes(`new ${P}(2)`) || armed.includes(`new ${P}(3)`) || armed.includes(`new ${P}(4)`))
    die(`the reader MARKED a name inside a comment, a string or a template. It is renaming text a page ` +
        `never evaluates, which would retire true rows.\n--- output ---\n${armed}`);
  if (armed.includes(`var ${P} =`) || armed.includes(`${P}.x`))
    die(`the reader marked a name BOUND by the enclosing scope. A bundle's own local is not the platform's ` +
        `name, and marking it would count a page's own variable as a platform read.\n--- output ---\n${armed}`);
  /* THE BRACKET QUOTED KEY IS ARMED IN ITS OWN TRANSFORM, BECAUSE IN A SHARED BLOB IT HAS NO EXPECTATION OF
     ITS OWN. esbuild re-prints `window["X"]` as `window.X`, which is the SAME text the dot-property
     expectation matches — so above, the two positions are one string and the dot line certifies the bracket
     line. Measured: deleting the `window["${probe}"]` occurrence from the control source and changing nothing
     else leaves the arming PASSING. That is absentrank's own `global["X"]` channel standing unarmed, and an
     unarmed position is where a silent retirement comes from. Alone, only a bracket key can put `window.` and
     the mark in this output. */
  const brack = await transform(`window["${probe}"];\nwindow['${probe}'];`);
  if (!brack.includes(`window.${P}`))
    die(`the reader did not mark a BRACKET quoted key, which is the position absentrank's \`global["X"]\` ` +
        `channel reads. A caller taking its occurrences as bare would retire genuine feature ` +
        `detections.\n--- output ---\n${brack}`);
  /* The control is DEFINED like every other name; that it would SPEAK is shown rather than assumed, because a
     control which has never produced a finding is not a control and its zero over a corpus would be a
     question nobody asked rather than a measured zero. */
  const ctl = await transform(`new ${CONTROL_NAME}(1);\nwindow.${CONTROL_NAME};\n"${CONTROL_NAME}" in window;`);
  if (!ctl.includes(CONTROL_NAME + MARK))
    die(`the control name is not markable, so a zero from it over a corpus measures nothing — it would read ` +
        `as "this name is never evaluated here" when it means "this name was never renamed at ` +
        `all".\n--- output ---\n${ctl}`);
  /* The one position it does NOT cover, armed so the claim is a measurement rather than a sentence: a
     template-literal key is outside mangleQuoted, and a caller reading it as not-code retires real guards. */
  const tick = await transform(`\x60${probe}\x60 in window; window[\x60${probe}\x60];`);
  if (!tick.includes(`\x60${probe}\x60 in window`))
    die(`a template-literal key was renamed after all, so quotedKeyJudgeable() is refusing a position this ` +
        `reader can now judge — the caller is floored for no reason. Re-measure it and delete the ` +
        `refusal.\n--- output ---\n${tick}`);

  /* THE BIND PROBE IS ARMED IN BOTH DIRECTIONS TOO, AND ITS NEGATIVES ARE THE FORMS IT EXISTS TO REFUSE. A
     probe that answered BOUND for everything would call a page's own DATA a binding, which moves an
     occurrence into the caller's `shadow` column and out of the one whose sites a reader is told to open;
     one that answered FREE for everything would leave the caller exactly where it was and read as a probe
     that found nothing. The last negative is this entry's own stated LIMIT rather than a defect: a binding
     in an INNER scope does not bind the appended reference, and the caller's own binder is the floor for it. */
  {
    const P = probe;
    const ask = async (src) => {
      const out = await transform(src + bindProbe([P]));
      const i = out.lastIndexOf(JSON.stringify(BIND_SENTINEL));
      if (i < 0)
        die(`the bind probe's sentinel did not survive its own control, so the tail cannot be located and ` +
            `every name would read BOUND.\n--- output ---\n${out}`);
      return markedIn(out.slice(i), P) ? "free" : "bound";
    };
    const WANT = [
      [`var ${P};`, "bound"], [`var ${P};${P} ||= {};`, "bound"], [`let ${P} = 1;`, "bound"],
      [`const ${P} = 1;`, "bound"], [`function ${P}(){}`, "bound"], [`class ${P}{}`, "bound"],
      [`var {${P}} = q;`, "bound"], [`var [${P}] = q;`, "bound"], [`import {${P}} from "m";`, "bound"],
      [`var s = "${P}";`, "free"], [`var s = \`${P}\`;`, "free"], [`/* ${P} */ var q = 1;`, "free"],
      [`${P}.x;`, "free"], [``, "free"],
      [`function g(){ var ${P}; return ${P}.x }`, "free"],   /* the stated LIMIT: an inner scope */
    ];
    for (const [src, want] of WANT) {
      const got = await ask(src);
      if (got !== want)
        die(`the bind probe answered ${got} for ${JSON.stringify(src)} and must answer ${want}. ` +
            (want === "bound"
              ? `A top-level binding form it cannot see is a page's own name reported as the platform's.`
              : `A string, a template, a comment or a binding in an INNER scope, reported as a top-level ` +
                `binding, moves an occurrence into a column that says the page owns the name.`));
    }
  }

  return {
    MARK, excluded, control: CONTROL_NAME,
    /* Returns the re-printed program with every evaluated reference marked, or {parsed:false} and the
       parser's own message. Nothing is claimed about a program that does not parse. */
    async read(src) {
      if (src.includes(MARK))
        die(`a source already spells ${MARK}, so a mark this reader placed cannot be told from the ` +
            `bundle's own text. Change MARK rather than filtering the file out.`);
      try { return { parsed: true, text: await transform(src) }; }
      catch (e) { return { parsed: false, text: null, why: String((e && e.message) || e).split("\n")[1] || String(e) }; }
    },
    /* Which of `names` the program BINDS at its top level, from the same resolution. `{parsed:false}` where
       the probed program does not parse, and nothing is claimed about it — the caller's floor decides. */
    async bindsTopLevel(src, names) {
      /* AN EXCLUDED NAME IS REFUSED RATHER THAN DROPPED, because it is the one input this entry answers
         WRONGLY AND SILENTLY: `window` is never defined, so the appended reference to it is never marked
         and every file would read BOUND — a page's own binding claimed for every corpus there is. A caller
         is told, exactly as `excluded` tells it about the rename. A reserved word cannot reach here at all;
         the constructor already died on one, so a filter for it would be a second answer to that. */
      const bad = names.filter((n) => GLOBAL_RECEIVERS.has(n));
      if (bad.length)
        die(`bindsTopLevel was asked about ${bad.join(", ")}, which this reader excludes from the rename. ` +
            `Nothing defines them, so the probe cannot mark them and every file would answer BOUND. A ` +
            `caller must read \`excluded\` and say so, never take this entry's answer for one.`);
      if (!names.length) return { parsed: true, bound: new Set() };
      const ask = names;
      let text;
      try { text = await transform(src + bindProbe(ask)); }
      catch (e) { return { parsed: false, bound: null, why: String((e && e.message) || e).split("\n")[1] || String(e) }; }
      const i = text.lastIndexOf(JSON.stringify(BIND_SENTINEL));
      if (i < 0)
        die(`the bind probe's own sentinel is absent from the re-print, so the tail cannot be located and ` +
            `every name would read BOUND — which is the direction that calls a page's data its own binding. ` +
            `esbuild is no longer emitting the appended statement; read the output before trusting this.`);
      const tail = text.slice(i);
      const bound = new Set();
      for (const n of ask) if (!markedIn(tail, n)) bound.add(n);
      return { parsed: true, bound };
    },
  };
}
