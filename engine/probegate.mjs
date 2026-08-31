/* PROBE-NEEDLE COLLISION AUDITOR — the gate `fork_row_impl`'s banner names as the thing that does not exist.
 *
 *   node engine/probegate.mjs [file …]   audit (default: engine/host/test_forced.c)
 *   node engine/probegate.mjs --census   the full classification of every site, not just the findings
 *   node engine/probegate.mjs --all      print every finding rather than the first 60 of each kind
 *
 * WHY THIS EXISTS. A fixture probe row spelled `strstr(js, "tok")` asks the WHOLE emitted result document
 * whether `tok` occurs anywhere in it. When some other statement of the same fixture emits a token that
 * CONTAINS `tok`, the row can read 1 with its own capability entirely broken — a green probe for a world the
 * run never observed. That failure has no symptom: the row passes, the document is well formed, and the only
 * thing that can say so is a scan that knows both what the rows ASK and what the documents can ANSWER.
 * Every instance found so far was found by hand, one at a time, by a lane that had already lost time to it:
 * `gone` inside four earlier statements' values, `afA`/`afP` inside `pafA`/`pafP`, `next` inside the location
 * sink's own `{state}.next` source shape, `proxy` inside `/api/hdrproxy`.
 *
 * BOTH SIDES ARE DERIVED FROM THE CODE THAT OWNS THEM — CLAUDE.md §an-auditor-derives-the-rule-it-checks.
 *   - the NEEDLES are the `strstr(<doc>, "…")` literals of the audited file, with C comments blanked first
 *     (a `strstr` NAMED in a comment is not a call, and counting one is how an earlier census reported 158);
 *   - `<doc>` is not a name this file types: it is the first parameter of the SCOPING PRIMITIVE the banner
 *     names as the fix (`param_value_is`), cross-checked against every other reader of the same document, so
 *     the day that parameter is renamed the gate follows it instead of silently auditing nothing;
 *   - the DOCUMENT TOKENS are read out of the fixture's own document literals, found by their SHAPE (a
 *     file-scope `static const char *X =` whose concatenated text is an HTML document with a `<script` in it),
 *     so a fourth fixture document added tomorrow is audited without editing this file;
 *   - the SOURCE SHAPES (`{state}.next`) are composed from the fixture's own `concolic_new(ctx, "{state}", …)`
 *     declaration and concolic.h's own `"%s.%s"` member composition — never a typed list of source names.
 *
 * WHAT IT REFUSES TO CALL A DEFECT, because each of these is the instrument being wrong rather than the file:
 *   - a WHOLE-JSON needle (`"\"X\""`) cannot sit inside `"XY"`: `json_buf_str` escapes an embedded quote, so
 *     both of the needle's own quote bytes are token boundaries in the emitted JSON;
 *   - a needle in a NEGATIVE position (`!strstr(...)`) can only be made unable to PASS by a superstring, never
 *     unable to FAIL, so a collision there is a false RED — a reading somebody checks — and the whole-document
 *     form is deliberate there;
 *   - a DECLARED PREFIX (`/*probegate:prefix*\/` at the site) is a needle whose only superstrings are the
 *     intended match. The declaration is CHECKED rather than obeyed: it holds only while every containing
 *     token still extends the needle to the right, and a second endpoint sharing the prefix re-reddens it.
 *
 * WHAT IT CANNOT SEE, stated because a silent zero is worse than a reported non-check. The token universe is
 * built from the fixture SOURCE, so a token the ENGINE synthesises out of thin air — a normalised URL
 * template, an error message, a census field name, a value built by concatenating a loop counter — is outside
 * it, and a needle contained by one of those is invisible here. The one synthesised family that IS covered is
 * the concolic source shape, because the fixture declares its roots in C and concolic.h owns the composition.
 * The count of needles this scan could not resolve to a literal at all (a runtime-composed `pat`) is printed
 * beside the findings for the same reason.
 *
 * IT IS NOT A BUILD GATE, for `citegen.mjs`'s reason: a fixture edit must not be able to stop every lane, and
 * a finding count that becomes a noise floor is a finding count nobody reads. */

import { readFileSync } from "node:fs";

const args = process.argv.slice(2);
const ALL = args.includes("--all");
const CENSUS = args.includes("--census");
const FILES = args.filter(a => !a.startsWith("--"));
if (FILES.length === 0) FILES.push("engine/host/test_forced.c");

/* ---------------------------------------------------------------- C lexing */

/* Comments blanked to spaces so every offset still names its original line. String and char literals are
 * skipped as units, so a `/*` inside a document literal is not a comment and a `"` inside a comment is not a
 * literal — both occur in the audited file. */
function blankComments(src) {
    const out = src.split("");
    let i = 0;
    while (i < src.length) {
        const c = src[i];
        if (c === '"' || c === "'") {
            const q = c;
            i++;
            while (i < src.length && src[i] !== q) i += src[i] === "\\" ? 2 : 1;
            i++;
            continue;
        }
        if (c === "/" && src[i + 1] === "*") {
            const end = src.indexOf("*/", i + 2);
            const stop = end < 0 ? src.length : end + 2;
            for (let k = i; k < stop; k++) if (out[k] !== "\n") out[k] = " ";
            i = stop;
            continue;
        }
        if (c === "/" && src[i + 1] === "/") {
            let k = i;
            while (k < src.length && src[k] !== "\n") out[k++] = " ";
            i = k;
            continue;
        }
        i++;
    }
    return out.join("");
}

function decodeCString(lit) {
    let s = "";
    for (let i = 0; i < lit.length; i++) {
        if (lit[i] !== "\\") { s += lit[i]; continue; }
        const e = lit[++i];
        if (e === "n") s += "\n";
        else if (e === "t") s += "\t";
        else if (e === "r") s += "\r";
        else if (e === "0") s += "\0";
        else if (e === "x") { let h = ""; while (/[0-9a-fA-F]/.test(lit[i + 1] || "")) h += lit[++i]; s += String.fromCharCode(parseInt(h, 16)); }
        else s += e;
    }
    return s;
}

/* Read a run of adjacent string literals starting at `i` (C concatenation), returning the decoded text and the
 * offset just past the last one. Returns null when `i` is not at a string literal. */
function readLiteralRun(src, i) {
    let text = "";
    const parts = [];
    for (;;) {
        while (i < src.length && /\s/.test(src[i])) i++;
        if (src[i] !== '"') break;
        const start = ++i;
        while (i < src.length && src[i] !== '"') i += src[i] === "\\" ? 2 : 1;
        const seg = decodeCString(src.slice(start, i));
        parts.push(seg); text += seg;
        i++;
    }
    return parts.length === 0 ? null : { text, end: i, parts };
}

const lineOf = (src, off) => src.slice(0, off).split("\n").length;

/* The top-level argument substrings of a call whose `(` is at `open`. */
function callArgs(src, open) {
    const args = [];
    let depth = 0, cur = "", i = open + 1;
    for (; i < src.length; i++) {
        const c = src[i];
        if (c === "(" || c === "[" || c === "{") depth++;
        else if (c === ")" || c === "]" || c === "}") { if (depth === 0) break; depth--; }
        else if (c === "," && depth === 0) { args.push(cur); cur = ""; continue; }
        cur += c;
    }
    args.push(cur);
    return args;
}

/* The decoded value of an argument that is exactly a string literal, else null. */
function argLiteral(a) {
    const t = a.trim();
    if (!t.startsWith('"')) return null;
    const run = readLiteralRun(t, 0);
    return run && run.end === t.length ? run.text : null;
}

/* ------------------------------------------------- side A: the needle list */

/* The name of the parameter holding the emitted result document, taken from the scoping primitive the
 * banner names as the fix. Every other reader of the same document must agree with it, or the gate has lost
 * track of which variable is the document and says so instead of auditing the wrong calls. */
function documentParamName(blank, path) {
    const owners = ["param_value_is", "param_values_span", "param_value_only", "param_value_has",
                    "param_value_count", "fork_row_impl"];
    const seen = new Map();
    for (const fn of owners) {
        const m = new RegExp(`\\b${fn}\\s*\\(\\s*const\\s+char\\s*\\*\\s*(\\w+)`).exec(blank);
        if (m) seen.set(fn, m[1]);
    }
    if (seen.size === 0) return null;
    const names = new Set(seen.values());
    if (names.size !== 1) {
        console.log(`${path}: the readers of the emitted document disagree about what it is called ` +
                    `(${[...seen].map(([f, n]) => `${f}:${n}`).join(", ")}) — this scan cannot tell which ` +
                    `strstr calls interrogate the result, so it audits none of them.`);
        return null;
    }
    return [...names][0];
}

function collectNeedles(src, blank, docName) {
    const sites = [];
    const re = /\bstrstr\s*\(/g;
    let m;
    while ((m = re.exec(blank)) !== null) {
        /* sign: consecutive `!` before the call, whitespace ignored. `!!x` is positive. */
        let j = m.index - 1, bangs = 0;
        while (j >= 0 && /\s/.test(blank[j])) j--;
        while (j >= 0 && blank[j] === "!") { bangs++; j--; while (j >= 0 && /\s/.test(blank[j])) j--; }

        /* first argument, up to the top-level comma */
        let i = m.index + m[0].length, depth = 0, arg = "";
        while (i < blank.length) {
            const c = blank[i];
            if (c === "(" || c === "[") depth++;
            else if (c === ")" || c === "]") { if (depth === 0) break; depth--; }
            else if (c === "," && depth === 0) break;
            arg += c; i++;
        }
        if (blank[i] !== ",") continue;                       /* single-argument strstr: not a probe */
        const subject = arg.trim();
        if (subject !== docName) continue;

        const run = readLiteralRun(blank, i + 1);
        /* A DECLARATION IS READ AT THE CALL IT DECLARES, never at its line: a row is several `strstr`s on one
         * line, and a line-scoped marker would exempt the neighbours a reader never meant it to. It is read
         * from the RAW source because `blank` has already erased every comment. */
        let close = run ? run.end : -1;
        while (close >= 0 && close < blank.length && /\s/.test(blank[close])) close++;
        const declaredPrefix = close >= 0 && blank[close] === ")" &&
                               /^\s*\/\*\s*probegate:prefix\s*\*\//.test(src.slice(close + 1, close + 48));
        sites.push({
            line: lineOf(src, m.index),
            negative: (bangs & 1) === 1,
            needle: run ? run.text : null,
            declaredPrefix,
        });
    }
    return sites;
}

/* ---------------------------------------------- side B: the token universe */

/* A fixture document is a file-scope `static const char *X =` whose concatenated literal text is an HTML
 * document that runs script. Found by shape so a new one needs no edit here. */
function collectDocuments(src, blank) {
    const docs = [];
    const re = /\bstatic\s+const\s+char\s*\*\s*(\w+)\s*=\s*/g;
    let m;
    while ((m = re.exec(blank)) !== null) {
        const run = readLiteralRun(blank, m.index + m[0].length);
        if (!run) continue;
        if (blank[run.end] !== ";") continue;
        if (!/<script/i.test(run.text)) continue;
        docs.push({ name: m[1], line: lineOf(src, m.index), text: run.text, parts: run.parts });
    }
    return docs;
}

/* The fixture's own concolic roots: `JS_SetPropertyStr(ctx, g, "state", concolic_new(ctx, "{state}", …))`
 * gives the JS name and the shape it displays as. concolic.h composes a member path as `"%s.%s"` over its
 * parent's shape, so `state.next` in a document is the token `{state}.next`. */
function collectConcolicRoots(blank) {
    const roots = new Map();
    const re = /JS_SetPropertyStr\s*\(\s*ctx\s*,\s*g\s*,\s*"([^"]+)"\s*,\s*concolic_new\s*\(\s*ctx\s*,\s*"([^"]+)"/g;
    let m;
    while ((m = re.exec(blank)) !== null) roots.set(m[1], m[2]);
    const bare = new Map();
    const re2 = /concolic_new\s*\(\s*ctx\s*,\s*"([^"]+)"/g;
    while ((m = re2.exec(blank)) !== null) bare.set(m[1], "shape");
    /* A concolic's fourth argument is its EXAMPLE, and an example is bytes the run puts in the document just
     * as a document literal is — so the C side of the fixture contributes tokens too. */
    const re3 = /concolic_new\s*\(\s*ctx\s*,\s*"[^"]*"\s*,\s*"[^"]*"\s*,\s*JS_NewString\s*\(\s*ctx\s*,\s*"([^"]+)"/g;
    while ((m = re3.exec(blank)) !== null) bare.set(m[1], "value");
    return { roots, bare };
}

/* Every token the run can put in the emitted document, derived from one fixture document's own text.
 *
 * A URL LITERAL IS DECOMPOSED, NEVER ADDED WHOLE, and that is the difference between a finding and a false
 * one: `endpoint.h` records a URL's PATH under `"url"` and its query as separate `name`/`validValues` pairs,
 * so the bytes `/api/touch?v=mouse` are never emitted adjacent and a needle `mouse` is contained by nothing.
 * Adding the raw literal as one token manufactured twelve findings that no run could produce.
 *
 * NESTED QUOTES ARE RE-SCANNED, because a document statement can put a whole other statement in a string
 * (`setTimeout("fetch('/api/timerstr?v=strran');", 0)`), and taking the outer string as one token makes its
 * inner URL a superstring of every value inside it.
 *
 * QUOTES ARE PAIRED WITHIN ONE SOURCE SEGMENT, NEVER ACROSS THE WHOLE DOCUMENT. The fixture writes one
 * statement per C string literal, and the document's PROSE carries apostrophes that no JS quote closes — so
 * pairing over the concatenation runs the parity off by one and every quote downstream of the stray one pairs
 * with its neighbour instead of its partner. Measured: that made `strran');` a token, and a token that is
 * half of one string and half of the next is a superstring of real values that nothing ever emits. */
function documentTokens(doc, roots, bare, add) {
    const strings = [];
    const harvest = (text, depth) => {
        const re = /'([^'\\]*(?:\\.[^'\\]*)*)'|"([^"\\]*(?:\\.[^"\\]*)*)"/g;
        let m;
        while ((m = re.exec(text)) !== null) {
            const s = m[1] !== undefined ? m[1] : m[2];
            strings.push(s);
            if (depth < 3 && /['"]/.test(s)) harvest(s, depth + 1);
        }
    };
    for (const seg of doc.parts) harvest(seg, 0);

    for (const s of strings) {
        if (s.length === 0) continue;
        /* A parameter NAME is introduced by `?` or `&` WHEREVER it appears, including in a bare `'&live='`
         * fragment a concatenation contributes — the emitted record spells it `"name":"live"` either way, and
         * a needle that equals it is answered from a field that is not the value. */
        for (const pm of s.matchAll(/[?&]([A-Za-z_][A-Za-z0-9_]*)=/g)) add(pm[1], "param", doc.name);
        /* A STRING THAT CARRIES QUOTES IS PROGRAM TEXT, NOT A DATUM. `setTimeout("fetch('…?v=strran');", 0)`
         * has a `?` in it, and treating it as a URL splits its query at the wrong end and mints `strran');`
         * — a token that is half a real value and half the code around it, and a superstring of the value the
         * row is actually about. Recurse into it and add nothing for the wrapper itself. */
        if (/['"]/.test(s)) continue;
        const q = s.indexOf("?");
        const isUrl = q >= 0 && (q === 0 || s.startsWith("/") || /^[a-z][a-z0-9+.\-]*:\/\//.test(s));
        if (!isUrl) {
            add(s, s.startsWith("/") && s.length > 1 && q < 0 ? "path" : "value", doc.name);
            continue;
        }
        const path = s.slice(0, q);
        if (path.startsWith("/") && path.length > 1) add(path, "path", doc.name);
        else if (path.length) add(path, "value", doc.name);
        for (const pair of s.slice(q + 1).split("&")) {
            const eq = pair.indexOf("=");
            const val = eq < 0 ? "" : pair.slice(eq + 1);
            if (val.length) add(val, "value", doc.name);
        }
    }
    for (const [name, shape] of roots) {
        add(shape, "shape", doc.name);
        const mre = new RegExp(`\\b${name}((?:\\.[A-Za-z_][A-Za-z0-9_]*)+)`, "g");
        let x;
        while ((x = mre.exec(doc.text)) !== null) add(shape + x[1], "shape", doc.name);
    }
    for (const [s, kind] of bare) add(s, kind, "C");
}

/* THE SCOPED ROWS ARE THEMSELVES A DECLARATION OF WHAT THE RUN EMITS, and they reach part of what the
 * document literals cannot. `param_value_is(js, "/api/rerepfork", "v", "xrrA1yrrA2")` states a value the
 * fixture never spells — the run BUILDS it out of a callback's two substitutions — so a bare needle contained
 * by it is invisible to a scan of the documents alone. Derived from the rows, never typed: the endpoint, the
 * parameter name and the expected value are the arguments those calls already carry. */
function expectedTokens(blank, add) {
    for (const fn of ["param_value_is", "param_value_only", "param_value_has"]) {
        const re = new RegExp(`\\b${fn}\\s*\\(`, "g");
        let m;
        while ((m = re.exec(blank)) !== null) {
            const a = callArgs(blank, m.index + m[0].length - 1);
            if (a.length < 4) continue;
            const url = argLiteral(a[1]), pname = argLiteral(a[2]), val = argLiteral(a[3]);
            if (url) add(url, "path", "row");
            if (pname) add(pname, "param", "row");
            if (val) add(val, "value", "row");
        }
    }
    const re = /\bFORK_ROW\s*\(/g;
    let m;
    while ((m = re.exec(blank)) !== null) {
        const a = callArgs(blank, m.index + m[0].length - 1);
        if (a.length < 8) continue;                       /* js, row, why, url, pname, subject, w0, w1 … */
        const url = argLiteral(a[3]), pname = argLiteral(a[4]);
        if (url) add(url, "path", "row");
        if (pname) add(pname, "param", "row");
        for (let k = 6; k < a.length; k++) { const w = argLiteral(a[k]); if (w) add(w, "value", "row"); }
    }
}

/* ------------------------------------------------------------------ audit */

let hardTotal = 0, unaudited = 0;
for (const path of FILES) {
    const src = readFileSync(path, "utf8");
    const blank = blankComments(src);

    const docName = documentParamName(blank, path);
    if (!docName) { unaudited++; console.log(`${path}: no emitted-document reader found — nothing audited.`); continue; }

    const docs = collectDocuments(src, blank);
    const { roots, bare } = collectConcolicRoots(blank);
    const tokens = new Map();                       /* token -> { kinds:Set, docs:Set } */
    const add = (t, kind, doc) => {
        let e = tokens.get(t);
        if (!e) tokens.set(t, (e = { kinds: new Set(), docs: new Set() }));
        e.kinds.add(kind); e.docs.add(doc);
    };
    for (const d of docs) documentTokens(d, roots, bare, add);
    expectedTokens(blank, add);

    const sites = collectNeedles(src, blank, docName);
    /* A POSITIVE NEEDLE IS ITSELF A CLAIM THAT THE RUN EMITS THAT TOKEN, so the rows are a second, independent
     * statement of the token universe — and the one that reaches values the fixture never spells, because the
     * run BUILDS them (a callback's two substitutions, a reply field concatenated onto a tag). This is what
     * found `us-west-2` sitting inside its own neighbours' `us-west-2-bodyADMIN` and `chain2-us-west-2`, which
     * a scan of the document literals alone cannot see. A whole-JSON needle contributes its INNER text only
     * where that text is one clean JSON string — otherwise it is JSON structure and not a token. */
    for (const s of sites) {
        if (s.needle === null || s.negative) continue;
        if (s.needle.startsWith('"') && s.needle.endsWith('"') && s.needle.length >= 2) {
            const inner = s.needle.slice(1, -1);
            if (inner.length && !/["\\:]/.test(inner)) add(inner, "value", "row");
            continue;
        }
        add(s.needle, "value", "row");
    }

    const cls = { wholeJson: [], negative: [], nonLiteral: [], contained: [], crossField: [],
                  declared: [], declaredStale: [], unique: [] };

    for (const s of sites) {
        if (s.needle === null) { cls.nonLiteral.push(s); continue; }
        /* whole-JSON is asked BEFORE the sign, because it is a property of the needle and not of the term:
         * a both-quoted needle is safe in either position, and asking the sign first files four of them
         * under `negative` and makes this census disagree with a hand count by exactly that many. */
        if (s.needle.length >= 2 && s.needle.startsWith('"') && s.needle.endsWith('"')) {
            cls.wholeJson.push(s); continue;
        }
        if (s.negative) { cls.negative.push(s); continue; }
        const containers = [];
        for (const [t, e] of tokens)
            if (t !== s.needle && t.includes(s.needle)) containers.push({ t, e });
        /* cross-field: the needle IS a token, and that token is spelled as a parameter NAME somewhere, so
         * `"name":"X"` satisfies the row exactly as a value would. */
        const self = tokens.get(s.needle);
        const crossField = self && self.kinds.has("param");

        if (containers.length === 0) {
            if (crossField) cls.crossField.push({ ...s, kinds: [...self.kinds] });
            else cls.unique.push(s);
            continue;
        }
        if (s.declaredPrefix) {
            const allExtendRight = containers.every(c => c.t.startsWith(s.needle));
            if (allExtendRight && containers.length === 1) cls.declared.push({ ...s, containers });
            else cls.declaredStale.push({ ...s, containers });
            continue;
        }
        cls.contained.push({ ...s, containers, crossField });
    }

    const n = o => o.length;
    const literal = sites.length - n(cls.nonLiteral);
    console.log(`${path}`);
    console.log(`  document variable        ${docName}   (derived from param_value_is's own signature)`);
    console.log(`  fixture documents        ${docs.length}: ${docs.map(d => `${d.name}@${d.line}`).join(", ")}`);
    console.log(`  concolic roots           ${[...roots].map(([k, v]) => `${k}→${v}`).join(", ") || "none"}`);
    console.log(`  emittable tokens         ${tokens.size}`);
    console.log(`  strstr(${docName}, "…") sites   ${literal}   (+${n(cls.nonLiteral)} whose needle is not a literal)`);
    console.log(`    whole-JSON "X"         ${n(cls.wholeJson)}  (both bytes are token boundaries — safe)`);
    console.log(`    unscoped               ${literal - n(cls.wholeJson)}, of which:`);
    console.log(`      negative position    ${n(cls.negative)}  (a superstring can only make it fail EARLY — a false red)`);
    console.log(`      unique today         ${n(cls.unique)}  (no token contains it TODAY; a later statement can)`);
    console.log(`      declared prefix      ${n(cls.declared)}  (checked, still exact)`);
    console.log(`  NOT CHECKED: ${n(cls.nonLiteral)} needle(s) composed at runtime — this scan resolves none of them.`);
    /* The OTHER substring test in this file, named so its silence is not read as a clean bill. `param_value_has`
     * is scoped to one param of one endpoint and is therefore immune to a token some OTHER statement emits —
     * the axis above — but it remains a substring test WITHIN that param, so a sibling VALUE of the same param
     * can contain its needle. That is a different question and this scan does not ask it. */
    const hasSites = [...blank.matchAll(/\bparam_value_has\s*\(/g)].length;
    console.log(`  NOT CHECKED: ${hasSites} param_value_has() site(s) — scoped, so immune to this axis, but`);
    console.log(`               still a substring test against the OTHER values of the same param.`);
    console.log(`  FINDINGS`);
    console.log(`    CONTAINED              ${n(cls.contained)}  (reads 1 on a world it never observed)`);
    console.log(`    CROSS-FIELD            ${n(cls.crossField)}  (equals a token spelled as a param NAME)`);
    console.log(`    DECLARED-PREFIX STALE  ${n(cls.declaredStale)}  (the declaration is no longer true)`);
    console.log(`  NOT CHECKED: tokens the ENGINE synthesises (normalised URL templates, error text, census`);
    console.log(`               field names, a value built from a loop counter) are outside this universe.`);

    const show = (title, rows, fmt) => {
        if (rows.length === 0) return;
        console.log(`\n  ── ${title} ──`);
        for (const r of (ALL ? rows : rows.slice(0, 60))) console.log("  " + fmt(r));
        if (!ALL && rows.length > 60) console.log(`  … ${rows.length - 60} more (--all)`);
    };
    show("CONTAINED", cls.contained, r =>
        `${path}:${r.line}: strstr(${docName}, ${JSON.stringify(r.needle)})` +
        (r.crossField ? " is ALSO a parameter NAME, and is" : "") + ` contained by ` +
        r.containers.map(c => `${JSON.stringify(c.t)} [${[...c.e.kinds].join("/")} in ${[...c.e.docs].join(",")}]`).join(", "));
    show("CROSS-FIELD", cls.crossField, r =>
        `${path}:${r.line}: strstr(${docName}, ${JSON.stringify(r.needle)}) equals a token that is also a ` +
        `parameter NAME (${r.kinds.join("/")}) — "name":"${r.needle}" satisfies it as readily as a value`);
    show("DECLARED-PREFIX STALE", cls.declaredStale, r =>
        `${path}:${r.line}: declared prefix ${JSON.stringify(r.needle)} now matches ` +
        r.containers.map(c => JSON.stringify(c.t)).join(", "));
    if (CENSUS) {
        show("unscoped but unique today", cls.unique, r => `${path}:${r.line}: ${JSON.stringify(r.needle)}`);
        show("non-literal needles", cls.nonLiteral, r => `${path}:${r.line}: needle composed at runtime`);
    }
    hardTotal += n(cls.contained) + n(cls.crossField) + n(cls.declaredStale);
}
/* A zero that means "nothing was asked" must not print like a zero that means "nothing was found" —
 * §MEASURE-WHAT-THE-SHIPPED-PATH-WRITES: an absent count and a zero count are different facts. */
if (unaudited)
    console.log(`\nNOT AUDITED: ${unaudited} of ${FILES.length} file(s) — the scan could not identify the ` +
                `emitted-document variable, so its ${hardTotal} finding(s) are a fact about the OTHER files only.`);
else
    console.log(`\n${hardTotal} finding(s). This audits ONE axis: whether a probe needle can be answered by a ` +
                `token some OTHER statement of the same fixture emits.`);
process.exitCode = hardTotal > 0 || unaudited > 0 ? 1 : 0;
