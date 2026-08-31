/* PROBE-ROW AUDITOR — TWO AXES: whether a needle COLLIDES, and whether a row's clauses JOIN.
 *
 *   node engine/probegate.mjs [file …]   audit (default: engine/host/test_forced.c)
 *   node engine/probegate.mjs --census   the full classification of every site, not just the findings
 *   node engine/probegate.mjs --all      print every finding rather than the first 60 of each kind
 *
 * AXIS 1 — COLLISION. A fixture probe row spelled `strstr(js, "tok")` asks the WHOLE emitted result document
 * whether `tok` occurs anywhere in it. When some other statement of the same fixture emits a token that
 * CONTAINS `tok`, the row can read 1 with its own capability entirely broken — a green probe for a world the
 * run never observed. That failure has no symptom: the row passes, the document is well formed, and the only
 * thing that can say so is a scan that knows both what the rows ASK and what the documents can ANSWER.
 * Every instance found so far was found by hand, one at a time, by a lane that had already lost time to it:
 * `gone` inside four earlier statements' values, `afA`/`afP` inside `pafA`/`pafP`, `next` inside the location
 * sink's own `{state}.next` source shape, `proxy` inside `/api/hdrproxy`.
 *
 * AXIS 2 — JOIN, and it is a DIFFERENT QUESTION THAT NO AMOUNT OF AXIS 1 REACHES. Axis 1 asks about ONE
 * needle at a time, so a row can pass it on every clause it has and still assert nothing: a conjunction of
 * whole-document existences states only that the bytes are SOMEWHERE, never that they are on the same record.
 * Six clauses over six params of one endpoint, asked unscoped, are satisfied by a run that put all six values
 * on one param, or on six different endpoints, or that SWAPPED two of them — and each of those is a live
 * defect the row exists to catch. Every needle can be unique in the whole document and the join still
 * unstated, which is why the rows converted in this fixture kept reporting 0 findings before and after: what
 * was wrong in them was never on axis 1. A clause's SCOPE is its record key; two clauses are joined when
 * their scopes are equal and non-empty; a bare `strstr` has no scope and joins with nothing, itself included.
 *
 * BOTH SIDES ARE DERIVED FROM THE CODE THAT OWNS THEM — CLAUDE.md §an-auditor-derives-the-rule-it-checks.
 *   - the NEEDLES are the `strstr(<doc>, "…")` literals of the audited file, with C comments blanked first
 *     (a `strstr` NAMED in a comment is not a call, and counting one is how an earlier census reported 158);
 *   - `<doc>` is not a name this file types: it is the first parameter of the SCOPING PRIMITIVE the banner
 *     names as the fix (`param_value_is`), so the day that parameter is renamed the gate follows it instead
 *     of silently auditing nothing — AND ITS LOCAL ALIASES GO WITH IT, because `const char *ss = js;` is the
 *     same bytes and a scan that accepts only `js` drops those rows into no count at all;
 *   - a NEEDLE READ OUT OF A TABLE is N literals wearing one spelling, resolved from the file's own
 *     `static const char *const T[][K] = { … }`, because a loop over a table is how one `strstr` line becomes
 *     a hundred probe terms and reporting it as one unresolved needle understates the surface by the table's
 *     own length;
 *   - the DOCUMENT TOKENS are read out of the fixture's own document literals, found by their SHAPE (a
 *     file-scope `static const char *X =` whose concatenated text is an HTML document with a `<script` in it),
 *     so a fourth fixture document added tomorrow is audited without editing this file;
 *   - the SOURCE SHAPES (`{state}.next`) are composed from the fixture's own `concolic_new(ctx, "{state}", …)`
 *     declaration and concolic.h's own `"%s.%s"` member composition — never a typed list of source names;
 *   - the READERS of the document are found by their SIGNATURES (`static … f(const char *<doc>, …)`), and
 *     WHAT EACH ARGUMENT MEANS is read out of their BODIES rather than named here. A locator returns
 *     `const char *`, so it FINDS a record and its string parameters are that record's key; a predicate is
 *     scoped by whichever of its own parameters it forwards into a locator's key positions, and the rest is
 *     payload. Three hand-kept lists of names used to stand in this file — which readers state a token, which
 *     is record-scoped, which argument is the endpoint — and one of them named a reader that had been
 *     RENAMED, so the loop meant to pull the biggest probe table's endpoints into the token universe matched
 *     nothing and contributed a silent zero to the universe every bare needle is checked against. A regex
 *     that matches no call and a reader that states no token print identically, which is why the second copy
 *     had to go rather than be corrected.
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
 * A needle this scan cannot resolve to a literal at all is printed with the `snprintf` FORMAT that fills it,
 * for the same reason: "5 needles composed at runtime" is a number, and `"\"url\":\"%s\""` is a reader being
 * able to see that the needle is anchored on both sides by JSON structure and decide for themselves.
 * NEITHER IS A COUNT OF PROBE TERMS, AND THE TWO DIFFER IN BOTH DIRECTIONS, so each gets its own line: a
 * table subscript is one call and N terms, and a `strstr` over something that is not the emitted document is
 * a call and NO term — that one used to leave no trace whatever, which is the excluded-test defect one level
 * down, since the rows the scan never saw were not in its denominator either.
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

/* A STRING LITERAL IS ONE TOKEN TO EVERY SPLITTER HERE, and forgetting it splits a VALUE rather than an
 * argument. `param_value_is(js, "/api/dataset", "read", "roleName,userId,x")` and a table row
 * `{ "\"/api/dataset\"", "roleName,userId,x" }` both carry a comma the emitter really writes, so a splitter
 * that reads it as a separator hands back an argument list of the wrong length — which this file then reports
 * as a needle it could not resolve, i.e. as a hole in the SCAN rather than the split it actually was.
 * Advances past the literal beginning at `i`, or returns `i` unchanged when there is none. */
function skipLiteral(src, i) {
    const q = src[i];
    if (q !== '"' && q !== "'") return i;
    i++;
    while (i < src.length && src[i] !== q) i += src[i] === "\\" ? 2 : 1;
    return i + 1;
}

/* The top-level argument substrings of a call whose `(` is at `open`. */
function callArgs(src, open) {
    const args = [];
    let depth = 0, cur = "", i = open + 1;
    for (; i < src.length;) {
        const c = src[i];
        if (c === '"' || c === "'") { const j = skipLiteral(src, i); cur += src.slice(i, j); i = j; continue; }
        if (c === "(" || c === "[" || c === "{") depth++;
        else if (c === ")" || c === "]" || c === "}") { if (depth === 0) break; depth--; }
        else if (c === "," && depth === 0) { args.push(cur); cur = ""; i++; continue; }
        cur += c; i++;
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

/* The top-level comma-separated elements of a braced initializer whose `{` is at `open`, and the offset just
 * past its matching `}`. Nested braces come back whole, so a 2-D table's rows arrive as `{ "a", "b" }`. */
function braceItems(src, open) {
    const items = [];
    let depth = 0, cur = "", i = open + 1;
    for (; i < src.length;) {
        const c = src[i];
        if (c === '"' || c === "'") { const j = skipLiteral(src, i); cur += src.slice(i, j); i = j; continue; }
        if (c === "{" || c === "(" || c === "[") depth++;
        else if (c === "}" || c === ")" || c === "]") { if (c === "}" && depth === 0) break; depth--; }
        else if (c === "," && depth === 0) { items.push(cur); cur = ""; i++; continue; }
        cur += c; i++;
    }
    if (cur.trim().length) items.push(cur);
    return { items, end: i + 1 };
}

/* ------------------------------------------------- side A: the needle list */

/* The name of the parameter holding the emitted result document, taken from the SCOPING PRIMITIVE the banner
 * names as the fix. It is one name read from one signature, so the day that parameter is renamed the gate
 * follows it. A LIST OF THE OTHER READERS USED TO STAND HERE and is gone: it was a second copy of a fact the
 * file already states in its own declarations, it had to be edited every time a reader was added or removed,
 * and the drift it was watching for is caught better by the derivation below — a reader that stopped calling
 * the document by this name lands in the NOT-A-PROBE line, by name and with a count, instead of silently
 * agreeing. */
function documentParamName(blank) {
    const m = /\bparam_value_is\s*\(\s*const\s+char\s*\*\s*(\w+)/.exec(blank);
    return m ? m[1] : null;
}

/* EVERY FILE-LOCAL READER OF THAT DOCUMENT, AND WHAT EACH ONE'S ARGUMENTS MEAN — derived from the file's own
 * signatures and bodies, so a reader added or renamed tomorrow is modelled without editing this file.
 *
 * THREE LISTS OF NAMES USED TO STAND HERE and every one of them was a second copy of a fact the audited file
 * already declares in C: which readers state a token, which reader is record-scoped, and which argument of
 * each is the endpoint. The second copy is what rots. One of them named a reader that had since been renamed,
 * so the loop meant to pull the biggest probe table's endpoints into the token universe matched NOTHING and
 * contributed a silent zero — to the very universe every bare needle in the file is checked against. Nothing
 * said so: a regex that matches no call and a reader that states no token print the same way.
 *
 * The derivation is the one the audited file performs on itself. A LOCATOR returns `const char *` and takes
 * the emitted document: its job is to FIND a record, so every `const char *` parameter it carries is part of
 * that record's KEY. A PREDICATE returns something else: it is scoped by whichever of its OWN parameters it
 * forwards into a locator's key positions, and whatever is left over is its PAYLOAD — the bytes it looks for
 * INSIDE the record it located. Resolved to a fixed point, because a predicate can be scoped through another
 * predicate, and a locator through another locator.
 *
 * THE KIND OF EACH KEY PARAMETER FALLS OUT OF THE SAME CHAIN and is not a table either. A key parameter that
 * is forwarded onward inherits the kind of the parameter it feeds; one that a LOCATOR introduces and forwards
 * nowhere is that locator's own NARROWING key, which is a field name within the record; and the locator at the
 * bottom of the chain forwards nothing at all, so its keys are the record's ADDRESS. That is exactly how the
 * audited file reads: an endpoint's `url` addresses the record, its `pname` narrows to one param inside it,
 * and the value is payload. */
function collectReaderModel(blank, docName) {
    const model = new Map();
    const re = /\bstatic\s+([A-Za-z_][\w \t*]*?)(\w+)\s*\(/g;
    let m;
    while ((m = re.exec(blank)) !== null) {
        const open = m.index + m[0].length - 1;
        const params = callArgs(blank, open).map(p => p.trim());
        const p0 = /^const\s+char\s*\*\s*(\w+)$/.exec(params[0] || "");
        if (!p0 || p0[1] !== docName) continue;

        /* the definition's own extent, so its header and its recursive calls are not counted as call sites */
        let j = open, d = 0;
        for (; j < blank.length; j++) { if (blank[j] === "(") d++; else if (blank[j] === ")" && --d === 0) { j++; break; } }
        while (j < blank.length && /\s/.test(blank[j])) j++;
        if (blank[j] !== "{") continue;                 /* a prototype, not a definition — it has no body to read */
        let bd = 0, k = j;
        while (k < blank.length) {
            const c = blank[k];
            if (c === '"' || c === "'") { k = skipLiteral(blank, k); continue; }
            if (c === "{") bd++;
            else if (c === "}" && --bd === 0) { k++; break; }
            k++;
        }
        const strPos = [];
        for (let q = 1; q < params.length; q++) if (/^const\s+char\s*\*\s*\w+$/.test(params[q])) strPos.push(q);
        model.set(m[2], {
            name: m[2], start: m.index, end: k, bodyStart: j,
            names: params.map(p => (/(\w+)\s*$/.exec(p) || [, null])[1]),
            strPos, locator: /char\s*\*\s*$/.test(m[1]),
            scope: null, kind: new Map(), calls: 0, down: [],
        });
    }

    /* the doc-forwarding calls each reader makes, read once */
    const forwards = new Map();
    for (const [name, r] of model) {
        const list = [];
        for (const [cn, c] of model) {
            if (cn === name) continue;
            const cre = new RegExp(`\\b${cn}\\s*\\(`, "g");
            let x;
            while ((x = cre.exec(blank)) !== null) {
                if (x.index < r.bodyStart || x.index >= r.end) continue;
                const a = callArgs(blank, x.index + x[0].length - 1).map(s => s.trim());
                if (a[0] !== docName) continue;
                list.push({ callee: cn, args: a });
            }
        }
        forwards.set(name, list);
    }

    /* THE BOTTOM OF THE CHAIN FORWARDS THE DOCUMENT TO NOBODY: it does the `strstr` itself, against a pattern
     * it builds from its own parameters, and there is nothing beneath it to inherit from. */
    const patternOf = r => {
        const re = /\bsnprintf\s*\(/g;
        re.lastIndex = r.bodyStart;
        let m;
        while ((m = re.exec(blank)) !== null && m.index < r.end) {
            const a = callArgs(blank, m.index + m[0].length - 1);
            if (a.length < 3) continue;
            const fmt = argLiteral(a[2]);
            if (fmt !== null) return { fmt, args: a.slice(3).map(s => s.trim()) };
        }
        return null;
    };
    const bottoms = [];
    for (const [name, r] of model)
        if (forwards.get(name).length === 0) { r.pattern = patternOf(r); bottoms.push(r); }
    for (const r of bottoms)
        if (r.locator) { r.scope = r.strPos.slice(); for (const p of r.scope) r.kind.set(p, "path"); }
    /* A BOTTOM READER THAT IS NOT A LOCATOR MATCHES THE RECORD AND THE FIELD IN ONE `strstr`, so its key is
     * the part of its pattern that a locator's pattern ALREADY IS. The audited file states that relation
     * itself, by building the longer format out of the shorter one's bytes — an @S PoC row's pattern is the
     * @S record's pattern with one more field appended, which is exactly why the row is prefix-anchored on
     * its payload and record-scoped on everything before it. Nothing here is typed: the prefix test is the
     * whole derivation, and a bottom predicate whose pattern extends no locator's stays unresolved and is
     * REPORTED, never handed a key it did not earn. */
    for (const r of bottoms) {
        if (r.scope !== null || !r.pattern) continue;
        const base = bottoms.find(b => b !== r && b.locator && b.pattern && b.scope &&
                                       r.pattern.fmt.startsWith(b.pattern.fmt));
        if (!base) continue;
        const scope = [], kind = new Map();
        for (let q = 0; q < base.pattern.args.length && q < base.scope.length; q++) {
            const idx = r.names.indexOf(r.pattern.args[q]);
            if (idx > 0 && r.strPos.includes(idx)) { scope.push(idx); kind.set(idx, base.kind.get(base.scope[q])); }
        }
        r.scope = scope; r.kind = kind;
    }
    for (let grew = true; grew;) {
        grew = false;
        for (const [name, r] of model) {
            if (r.scope !== null) continue;
            const fw = forwards.get(name).filter(f => model.get(f.callee).scope !== null);
            if (fw.length === 0 || fw.length !== forwards.get(name).length) continue;
            const scope = new Set(), kind = new Map();
            for (const f of fw) {
                const c = model.get(f.callee);
                /* THE MAPPING IS RECORDED, NOT RE-DERIVED BY ORDER LATER. Aligning a caller's keys with a
                 * callee's by position assumes the two lists run in the same order, and nothing in C says
                 * they must; a needle inside the callee is then substituted with the caller's arguments in
                 * the wrong slots, which is a concrete term that reads plausible and searches for bytes no
                 * run emits. Keep the pairs the derivation actually established. */
                const pairs = [];
                for (const p of c.scope) {
                    const idx = r.names.indexOf(f.args[p]);
                    if (idx > 0 && r.strPos.includes(idx)) {
                        scope.add(idx); kind.set(idx, c.kind.get(p)); pairs.push([idx, p]);
                    }
                }
                if (pairs.length) r.down.push({ callee: f.callee, pairs });
            }
            r.scope = [...scope].sort((x, y) => x - y);
            r.kind = kind;
            /* a LOCATOR narrows: a key it introduces and forwards nowhere names a field inside the record */
            if (r.locator) for (const p of r.strPos) if (!scope.has(p)) { r.scope.push(p); r.kind.set(p, "param"); }
            r.scope.sort((x, y) => x - y);
            grew = true;
        }
    }
    /* A READER THIS CANNOT RESOLVE IS UNSCOPED AND SAYS SO — never quietly given a scope it did not earn,
     * because a wrong scope makes an unjoined row read as a joined one, which is the finding inverted. The
     * flag is what the report prints: a reader carrying string parameters whose key this could not derive is
     * a HOLE IN THE JOIN AXIS, and every row that goes through it is being judged unscoped for a reason that
     * is about this scan and not about the fixture. That distinction is the whole point of naming it. */
    for (const r of model.values()) {
        if (r.scope === null) { r.scope = []; r.unresolved = r.strPos.length > 0; }
        r.payload = r.strPos.filter(p => !r.scope.includes(p));
        for (const p of r.payload) r.kind.set(p, "value");
    }
    for (const [name, r] of model) {
        const cre = new RegExp(`\\b${name}\\s*\\(`, "g");
        let x;
        while ((x = cre.exec(blank)) !== null) if (x.index < r.start || x.index >= r.end) r.calls++;
    }
    return model;
}

/* EVERY NAME THAT IS THE EMITTED DOCUMENT, not only the one the primitive's signature spells. A file-local
 * `const char *ss = js;` makes `ss` the same bytes, and a scan that accepts only `js` DROPS those rows —
 * silently, because the subject test `continue`s and the site lands in no count at all. That is
 * §A-TEST-FILE-THE-GATE-DOES-NOT-COLLECT one level down: the total looks complete because the rows it never
 * saw are not in the denominator. Followed transitively so an alias of an alias is still the document. */
/* AND AN ALIAS IS SCOPED, because a name is not a variable. `param_values_span` walks a local `p` INSIDE the
 * document and a probe row a thousand lines away walks its own `p` ACROSS it, and a file-wide name set cannot
 * tell them apart — it audits the helper's own structural scan as if it were a probe term and reports a
 * finding about a `strstr` no statement of the fixture owns. So an alias reaches only to the end of the
 * top-level brace block it was declared in, which is the function. */
function topLevelBlocks(blank) {
    const spans = [];
    let depth = 0, start = -1;
    for (let i = 0; i < blank.length;) {
        const c = blank[i];
        if (c === '"' || c === "'") { i = skipLiteral(blank, i); continue; }
        if (c === "{") { if (depth++ === 0) start = i; }
        else if (c === "}") { if (--depth === 0) spans.push({ start, end: i }); if (depth < 0) depth = 0; }
        i++;
    }
    return spans;
}

function documentAliases(blank, docName) {
    const spans = topLevelBlocks(blank);
    /* the document parameter itself is in scope everywhere this scan looks */
    const aliases = [{ name: docName, start: 0, end: blank.length }];
    const re = /\bconst\s+char\s*\*\s*(\w+)\s*=\s*(\w+)\s*[;)]/g;
    for (let grew = true; grew;) {
        grew = false;
        re.lastIndex = 0;
        let m;
        while ((m = re.exec(blank)) !== null) {
            const b = spans.find(s => m.index > s.start && m.index < s.end);
            const scope = b ? { start: m.index, end: b.end } : { start: m.index, end: blank.length };
            const covers = aliases.some(a => a.name === m[2] && m.index >= a.start && m.index <= a.end);
            const already = aliases.some(a => a.name === m[1] && a.start === scope.start);
            if (covers && !already) { aliases.push({ name: m[1], ...scope }); grew = true; }
        }
    }
    return aliases;
}

const isDocument = (aliases, name, off) =>
    aliases.some(a => a.name === name && off >= a.start && off <= a.end);

/* A NEEDLE READ OUT OF THE FILE'S OWN TABLE IS NOT A RUNTIME COMPOSITION — IT IS N LITERALS WEARING ONE
 * SPELLING, AND THAT IS HOW ONE LINE BECOMES A HUNDRED PROBE TERMS. A loop `strstr(js, T[i][1])` over a
 * file-scope table asks one question per row, and reporting it as a single unresolved needle understates the
 * unchecked surface by the table's own length — the opposite of what the "NOT CHECKED" line exists to do.
 * Read by SHAPE from the file's own declaration (`static const char *const NAME[…][…] = { … }`), so a table
 * added tomorrow is audited without editing this file, and an element that is not a plain literal comes back
 * null and stays unresolved rather than being guessed at. */
function collectTables(blank) {
    const tables = new Map();
    const re = /\bstatic\s+const\s+char\s*\*(?:\s*const)?\s*(\w+)\s*((?:\[[^\]]*\])+)\s*=\s*\{/g;
    let m;
    while ((m = re.exec(blank)) !== null) {
        const dims = (m[2].match(/\[/g) || []).length;
        const open = m.index + m[0].length - 1;
        const { items } = braceItems(blank, open);
        const rows = items.map(it => {
            const t = it.trim();
            if (dims >= 2 && t.startsWith("{")) return braceItems(t, 0).items.map(argLiteral);
            return [argLiteral(t)];
        });
        tables.set(m[1], rows);
    }
    return tables;
}

/* The literals a table-subscripted needle can be. `T[i][k]` is column k of every row; `T[i]` is every entry
 * of a one-wide table. A subscript this cannot read, or a name that is no table, answers null — which keeps
 * the site in the unresolved count instead of turning it into a smaller, prettier number. */
function tableNeedles(argText, tables) {
    const m = /^(\w+)\s*\[[^\]]*\]\s*(?:\[\s*(\d+)\s*\])?$/.exec(argText.trim());
    if (!m) return null;
    const rows = tables.get(m[1]);
    if (!rows || rows.length === 0) return null;
    const col = m[2] === undefined ? 0 : Number(m[2]);
    if (m[2] === undefined && rows.some(r => r.length !== 1)) return null;
    if (rows.some(r => col >= r.length)) return null;
    const vals = rows.map(r => r[col]);
    return vals.some(v => v === null) ? null : vals;
}

/* WHAT AN UNRESOLVED NEEDLE IS MADE OF, so the non-check is legible rather than merely counted. Each of these
 * is a local buffer an `snprintf` filled, and the FORMAT is a literal in the same file — printing it is the
 * difference between "this scan resolves none of them" and a reader being able to see that
 * `"\"url\":\"%s\""` is anchored on both sides by JSON structure. Derived by reading the last `snprintf` into
 * that identifier before this call; a needle no such write precedes reports as unformatted. */
function needleWrite(blank, ident, before) {
    const re = new RegExp(`\\bsnprintf\\s*\\(\\s*${ident}\\s*,`, "g");
    let m, open = -1;
    while ((m = re.exec(blank)) !== null && m.index < before) open = m.index + m[0].length - 2;
    if (open < 0) return null;
    const a = callArgs(blank, open);
    if (a.length < 3) return null;
    const fmt = argLiteral(a[2]);
    return fmt === null ? null : { fmt, args: a.slice(3).map(s => s.trim()) };
}

/* WHAT A RUNTIME-COMPOSED NEEDLE ACTUALLY LOOKS FOR, resolved rather than counted. Each of these sits inside
 * a reader, filled by an `snprintf` whose format is a literal in the same file and whose arguments are that
 * reader's own parameters — so the CALL SITES supply the missing bytes, and substituting them turns one
 * unresolved needle into the concrete terms the run really searches for. Those terms then go through the same
 * CONTAINED and CROSS-FIELD checks as every literal needle, which is the point: "5 needles composed at
 * runtime" was a silent zero in the denominator of the whole scan.
 * A CALL SITE THIS CANNOT SUBSTITUTE IS NAMED, NEVER DROPPED — by the argument that defeated it — because a
 * partial resolution reported as a whole one is the same defect one layer up. */
/* WHAT EACH READER'S KEY PARAMETERS ACTUALLY HOLD, propagated down the chain the model recorded. A row calls
 * `param_value_is(js, "/api/hdrs", "acc", …)`; the needle that searches for the record is three readers down,
 * inside `emitted_record_span`, whose OWN call sites are the two helpers above it and carry no literal at all.
 * Substituting a reader's direct callers therefore resolves NOTHING for exactly the readers that matter — the
 * ones the whole file funnels through — so the binding travels the same edges the scope derivation did. */
function keyBindings(model, clauses) {
    const bind = new Map();
    for (const r of model.keys()) bind.set(r, []);
    for (const c of clauses) if (c.fn && c.bind.size) bind.get(c.fn).push(c.bind);
    for (let grew = true; grew;) {
        grew = false;
        for (const [name, r] of model)
            for (const d of r.down)
                for (const b of bind.get(name)) {
                    const out = new Map();
                    for (const [up, down] of d.pairs) if (b.has(up)) out.set(down, b.get(up));
                    if (out.size === 0) continue;
                    const key = [...out].map(([k, v]) => `${k}=${v}`).sort().join("");
                    const have = bind.get(d.callee);
                    if (have.some(x => [...x].map(([k, v]) => `${k}=${v}`).sort().join("") === key)) continue;
                    have.push(out); grew = true;
                }
    }
    return bind;
}

function composedNeedles(blank, model, bind, ident, at) {
    const r = [...model.values()].find(x => at >= x.bodyStart && at < x.end);
    if (!r) return { why: "sits outside every modelled reader, so it has no call sites to substitute" };
    const w = needleWrite(blank, ident, at);
    if (!w) return { reader: r.name, why: `no snprintf into \`${ident}\` precedes this call` };
    const slots = w.fmt.split(/%[-#0-9.+ ]*s/);
    if (slots.length - 1 !== w.args.length)
        return { reader: r.name, fmt: w.fmt,
                 why: `the format carries conversions other than %s, which this scan does not substitute` };
    const needles = new Map(), blocked = new Set();
    const bs = bind.get(r.name) || [];
    for (const b of bs) {
        let s = slots[0], ok = true;
        for (let q = 0; q < w.args.length; q++) {
            const p = r.names.indexOf(w.args[q]);
            const v = p > 0 && b.has(p) ? b.get(p) : argLiteral(w.args[q]);
            if (v === null || v === undefined) { ok = false; blocked.add(w.args[q]); break; }
            s += v + slots[q + 1];
        }
        /* THE RECORD THIS TERM ADDRESSES, so containment between two views of ONE record is not reported as a
         * collision. A locator's pattern is a PREFIX of the pattern a longer reader of the same record builds
         * — `{"sink":"eval","source":"{state}.code"` inside `…,"poc":"';X9()//` — and a reader finding its
         * needle there has found exactly the record it was locating, which is the opposite of reading a world
         * it never observed. Two terms with the same key are the same record and are not each other's
         * container; a BARE needle has no key, so it is still checked against both. */
        if (ok) needles.set(s, r.scope.map(p => b.has(p) ? b.get(p) : "?").join(""));
    }
    return { reader: r.name, fmt: w.fmt, sites: bs.length, blocked: [...blocked],
             needles: [...needles].map(([text, rec]) => ({ text, rec })) };
}

function collectNeedles(src, blank, docNames, tables) {
    const sites = [], foreign = [];
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
        if (!isDocument(docNames, subject, m.index)) { foreign.push({ line: lineOf(src, m.index), subject }); continue; }

        const run = readLiteralRun(blank, i + 1);
        /* A DECLARATION IS READ AT THE CALL IT DECLARES, never at its line: a row is several `strstr`s on one
         * line, and a line-scoped marker would exempt the neighbours a reader never meant it to. It is read
         * from the RAW source because `blank` has already erased every comment. */
        let close = run ? run.end : -1;
        while (close >= 0 && close < blank.length && /\s/.test(blank[close])) close++;
        const declaredPrefix = close >= 0 && blank[close] === ")" &&
                               /^\s*\/\*\s*probegate:prefix\s*\*\//.test(src.slice(close + 1, close + 48));
        const line = lineOf(src, m.index), negative = (bangs & 1) === 1;
        if (run) { sites.push({ line, negative, needle: run.text, declaredPrefix }); continue; }

        /* The second argument's own text, for the two things that can still be read out of it. */
        let k = i + 1, depth2 = 0, second = "";
        while (k < blank.length) {
            const c = blank[k];
            if (c === "(" || c === "[") depth2++;
            else if (c === ")" || c === "]") { if (depth2 === 0) break; depth2--; }
            else if (c === "," && depth2 === 0) break;
            second += c; k++;
        }
        const fromTable = tableNeedles(second, tables);
        if (fromTable) {
            for (const t of fromTable) sites.push({ line, negative, needle: t, declaredPrefix, table: second.trim() });
            continue;
        }
        const expr = second.trim();
        sites.push({ line, negative, needle: null, declaredPrefix, expr, at: m.index });
    }
    return { sites, foreign };
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
 * parameter name and the expected value are the arguments those calls already carry.
 * A ROW MAY NAME ITS ENDPOINT AND ITS TOKEN OUT OF A TABLE, and a table row is a literal wearing a subscript
 * — so those are resolved here too, or the hundred-odd tokens the biggest probe table states would be missing
 * from the universe every bare needle in the file is checked against.
 * WHICH READERS STATE A TOKEN, AND WHICH ARGUMENT OF EACH DOES, IS NOT TYPED HERE — it is the reader model,
 * which reads it off the audited file's own signatures. A list of two or three names in this position is
 * exactly what let a renamed reader stop contributing tokens with nothing anywhere to say so. */
function expectedTokens(blank, tables, model, docName, add) {
    const lit = a => argLiteral(a) ?? (tableNeedles(a, tables) || null);
    const each = (v, kind) => { if (typeof v === "string") add(v, kind, "row");
                                else if (Array.isArray(v)) for (const s of v) add(s, kind, "row"); };
    for (const [fn, r] of model) {
        if (r.strPos.length === 0) continue;
        const re = new RegExp(`\\b${fn}\\s*\\(`, "g");
        let m;
        while ((m = re.exec(blank)) !== null) {
            if (m.index >= r.start && m.index < r.end) continue;
            const a = callArgs(blank, m.index + m[0].length - 1);
            if (a[0] === undefined || a[0].trim() !== docName) continue;
            for (const p of r.strPos) if (a[p] !== undefined) each(lit(a[p]), r.kind.get(p));
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

/* ------------------------------------------------- side C: the join axis */

/* WHETHER A ROW'S SEVERAL CLAUSES ARE ABOUT THE SAME RECORD — an axis with no instrument until now, and the
 * one the rows that have been converted here were actually wrong on.
 *
 * Everything above asks a question about ONE needle: can some OTHER statement of this fixture emit a token
 * that contains it. A row can pass that check on every clause it has and still assert nothing, because a
 * conjunction of whole-document existences states only that the bytes are SOMEWHERE. Six clauses over six
 * params of one endpoint, asked unscoped, are satisfied by a run that put all six values on one param, or on
 * six different endpoints, or that SWAPPED two of them — and each of those is a live defect the row exists to
 * catch. The collision axis is structurally unable to see it: every needle can be unique in the whole document
 * and the join still unstated.
 *
 * A CLAUSE'S SCOPE IS ITS RECORD KEY, taken from the reader model — the concrete arguments the row passes at
 * the key positions the audited file's own signatures put there. Two clauses are JOINED when their scopes are
 * equal and non-empty. A bare `strstr` over the document has no scope at all, so it joins with nothing,
 * including itself.
 *
 * A ROW IS ONE STATEMENT, because that is the unit the fixture asserts in: `int name = (a && b && c);`. Cut at
 * every `;` that is at paren depth zero, so a `for (;;)` header is one statement and not three. */
function collectStatements(blank) {
    const out = [];
    let start = 0, depth = 0;
    for (let i = 0; i < blank.length;) {
        const c = blank[i];
        if (c === '"' || c === "'") { i = skipLiteral(blank, i); continue; }
        if (c === "(" || c === "[") depth++;
        else if (c === ")" || c === "]") { if (depth > 0) depth--; }
        else if (depth === 0 && (c === ";" || c === "{" || c === "}")) {
            if (i > start) out.push({ start, end: i });
            start = i + 1;
        }
        i++;
    }
    return out;
}

/* Every clause of the file: a call to a modelled reader, or a bare `strstr`, whose subject is the document.
 * Scope and payload arguments are resolved to literals where they are literals and kept as their own source
 * text where they are not — an unresolved key is still a key, and two clauses passing the SAME expression are
 * still about the same record. */
function collectClauses(src, blank, model, docNames, tables) {
    const out = [];
    const lit = a => { const v = argLiteral(a); if (v !== null) return JSON.stringify(v);
                       const t = tableNeedles(a, tables); return t ? `[${t.join("")}]` : a.trim(); };
    for (const [name, r] of model) {
        const re = new RegExp(`\\b${name}\\s*\\(`, "g");
        let m;
        while ((m = re.exec(blank)) !== null) {
            if (m.index >= r.start && m.index < r.end) continue;
            const a = callArgs(blank, m.index + m[0].length - 1);
            if (a[0] === undefined || !isDocument(docNames, a[0].trim(), m.index)) continue;
            /* the literal each key parameter actually holds at this site, by POSITION — what a needle inside
             * a reader further down the chain is substituted with */
            const bind = new Map();
            for (const p of r.strPos) { const v = a[p] === undefined ? null : argLiteral(a[p]); if (v !== null) bind.set(p, v); }
            out.push({ at: m.index, line: lineOf(src, m.index), fn: name, bind,
                       scope: r.scope.filter(p => a[p] !== undefined).map(p => lit(a[p])),
                       payload: r.payload.filter(p => a[p] !== undefined).map(p => lit(a[p])) });
        }
    }
    const re = /\bstrstr\s*\(/g;
    let m;
    while ((m = re.exec(blank)) !== null) {
        const a = callArgs(blank, m.index + m[0].length - 1);
        if (a.length < 2 || !isDocument(docNames, a[0].trim(), m.index)) continue;
        out.push({ at: m.index, line: lineOf(src, m.index), fn: null, bind: new Map(),
                   scope: [], payload: [lit(a[1])] });
    }
    return out.sort((x, y) => x.at - y.at);
}

/* THE THREE WAYS A ROW'S CLAUSES FAIL TO BE ABOUT ONE RECORD, kept apart because the remedies differ and a
 * single number would name none of them.
 *
 * UNJOINED   two or more clauses with no scope at all. The row states N facts and ties none of them to a
 *            record, so one record carrying all N bytes satisfies it, and so does a seam that swapped which
 *            param carries which value. This is the shape every row converted in this fixture so far had.
 * DANGLING   one unscoped clause standing beside scoped ones. The scoped clauses name a record; the unscoped
 *            one can be answered BY THAT VERY RECORD, so it adds a claim the row already made rather than the
 *            new one it appears to add.
 * REPEATED   two clauses identical in reader, scope AND payload. One question asked twice reads as two facts,
 *            and the second answer is the first one. */
function classifyRow(cl) {
    const unscoped = cl.filter(c => c.scope.length === 0);
    const scoped = cl.filter(c => c.scope.length > 0);
    const full = c => `${c.fn ?? "strstr"}(${c.scope.join("")}|${c.payload.join("")})`;
    const seen = new Map(), repeated = [];
    for (const c of cl) {
        const k = full(c);
        if (seen.has(k)) repeated.push({ a: seen.get(k), b: c }); else seen.set(k, c);
    }
    return { unscoped, scoped, repeated,
             records: [...new Set(scoped.map(c => c.scope.join("")))] };
}

/* ------------------------------------------------------------------ audit */

let hardTotal = 0, unaudited = 0;
for (const path of FILES) {
    const src = readFileSync(path, "utf8");
    const blank = blankComments(src);

    const docName = documentParamName(blank);
    if (!docName) { unaudited++; console.log(`${path}: no emitted-document reader found — nothing audited.`); continue; }
    const docNames = documentAliases(blank, docName);
    const tables = collectTables(blank);
    const model = collectReaderModel(blank, docName);

    const docs = collectDocuments(src, blank);
    const { roots, bare } = collectConcolicRoots(blank);
    const tokens = new Map();                       /* token -> { kinds:Set, docs:Set } */
    const add = (t, kind, doc, rec) => {
        let e = tokens.get(t);
        if (!e) tokens.set(t, (e = { kinds: new Set(), docs: new Set(), recs: new Set() }));
        e.kinds.add(kind); e.docs.add(doc); if (rec !== undefined) e.recs.add(rec);
    };
    for (const d of docs) documentTokens(d, roots, bare, add);
    expectedTokens(blank, tables, model, docName, add);

    const { sites, foreign } = collectNeedles(src, blank, docNames, tables);

    /* A COMPOSED NEEDLE IS RESOLVED TO THE CONCRETE TERMS ITS CALL SITES MAKE OF IT, and each of those is then
     * an ordinary needle this scan checks. What cannot be substituted stays in `sites` as itself, so the
     * unresolved count is what is genuinely unresolved rather than what was never attempted. */
    /* the join axis: one statement is one row, and a row with a single clause has no join to state */
    const allClauses = collectClauses(src, blank, model, docNames, tables);
    const bind = keyBindings(model, allClauses);

    const composed = [];
    for (let q = sites.length - 1; q >= 0; q--) {
        const s = sites[q];
        if (s.needle !== null || !/^\w+$/.test(s.expr || "")) continue;
        const c = composedNeedles(blank, model, bind, s.expr, s.at);
        composed.push({ ...s, ...c });
        if (!c.needles || c.needles.length === 0) continue;
        sites.splice(q, 1, ...c.needles.map(t =>
            ({ line: s.line, negative: s.negative, needle: t.text, rec: t.rec,
               declaredPrefix: s.declaredPrefix, composedFrom: s.expr })));
    }

    const joinRows = [];
    for (const st of collectStatements(blank)) {
        const cl = allClauses.filter(c => c.at >= st.start && c.at < st.end);
        if (cl.length < 2) continue;
        joinRows.push({ line: lineOf(src, cl[0].at), n: cl.length, ...classifyRow(cl) });
    }
    const unjoined = joinRows.filter(r => r.unscoped.length >= 2);
    const dangling = joinRows.filter(r => r.unscoped.length === 1 && r.scoped.length >= 1);
    const repeated = joinRows.filter(r => r.repeated.length > 0);
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
            if (inner.length && !/["\\:]/.test(inner)) add(inner, "value", "row", s.rec);
            continue;
        }
        add(s.needle, "value", "row", s.rec);
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
        for (const [t, e] of tokens) {
            if (t === s.needle || !t.includes(s.needle)) continue;
            /* two views of ONE record are not each other's container — see composedNeedles */
            if (s.rec !== undefined && e.recs.has(s.rec)) continue;
            containers.push({ t, e });
        }
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
    const fromTable = sites.filter(s => s.table).length;
    const tableNames = [...new Set(sites.filter(s => s.table).map(s => s.table))];
    console.log(`${path}`);
    console.log(`  document variable        ${docName}   (derived from param_value_is's own signature)`);
    console.log(`  …and its aliases         ${docNames.slice(1).map(a => `${a.name}@${lineOf(src, a.start)}`).join(", ") || "none"}`);
    console.log(`  fixture documents        ${docs.length}: ${docs.map(d => `${d.name}@${d.line}`).join(", ")}`);
    console.log(`  concolic roots           ${[...roots].map(([k, v]) => `${k}→${v}`).join(", ") || "none"}`);
    console.log(`  emittable tokens         ${tokens.size}`);
    console.log(`  strstr(<doc>, "…") terms   ${literal}   (+${n(cls.nonLiteral)} whose needle is not a literal)`);
    console.log(`    of which read from a table  ${fromTable}${tableNames.length ? `: ${tableNames.join(", ")}` : ""}`);
    console.log(`    whole-JSON "X"         ${n(cls.wholeJson)}  (both bytes are token boundaries — safe)`);
    console.log(`    unscoped               ${literal - n(cls.wholeJson)}, of which:`);
    console.log(`      negative position    ${n(cls.negative)}  (a superstring can only make it fail EARLY — a false red)`);
    console.log(`      unique today         ${n(cls.unique)}  (no token contains it TODAY; a later statement can)`);
    console.log(`      declared prefix      ${n(cls.declared)}  (checked, still exact)`);
    /* A COUNT OF `strstr` CALLS IS NOT A COUNT OF PROBE TERMS, and the two differ in BOTH directions — which
     * is why each direction gets its own line rather than one number that could be read as either. A table
     * subscript is ONE call and N terms; a scan over something that is not the emitted document is a call and
     * NO term, and used to leave no trace at all. */
    console.log(`  NOT A PROBE: ${foreign.length} strstr call(s) whose subject is not the emitted document ` +
                `(${[...new Set(foreign.map(f => f.subject))].join(", ") || "none"}).`);
    /* COMPOSED NEEDLES, RESOLVED OR NAMED. A count on its own was a silent zero in the denominator of every
     * other number here; a format on its own let a reader judge the anchoring by eye. Substituting the call
     * sites is what puts these terms through the SAME checks as every literal needle, and what remains is a
     * residue with a name and an argument attached rather than an integer. */
    const resolvedTerms = composed.reduce((a, c) => a + (c.needles ? c.needles.length : 0), 0);
    console.log(`  COMPOSED NEEDLES: ${composed.length} site(s) → ${resolvedTerms} concrete term(s), checked above:`);
    for (const c of composed) {
        const head = `               ${path}:${c.line}: ${c.expr} in ${c.reader ?? "?"}`;
        if (!c.fmt) { console.log(`${head} — NOT RESOLVED: ${c.why}`); continue; }
        if (c.why) { console.log(`${head} ${JSON.stringify(c.fmt)} — NOT RESOLVED: ${c.why}`); continue; }
        console.log(`${head} ${JSON.stringify(c.fmt)} — ${c.needles.length} term(s) from ${c.sites} call site(s)` +
                    (c.blocked.length ? `; NOT RESOLVED where ${c.blocked.join(", ")} is not a literal` : "") +
                    (c.sites === 0 ? "; NO call site passes the document by name (a macro fills it)" : ""));
    }
    /* THE ROWS THAT ARE NOT `strstr` AT ALL, so their silence is not read as a clean bill. Each of these is
     * scoped and therefore immune to the axis above — no byte of another statement's record is in range — but
     * a reader that is a SUBSTRING test inside its own scope still cannot refuse a superstring there, and
     * naming the readers with their call counts is what tells a lane how much of this file is asked at which
     * address. Derived from the file's own signatures; the previous line named one reader by hand. */
    console.log(`  READERS OF <doc> (calls, and what the file's own signatures say each argument is):`);
    for (const r of [...model.values()].sort((a, b) => b.calls - a.calls))
        console.log(`               ${r.name.padEnd(22)}${String(r.calls).padStart(3)}  ` +
                    `${r.locator ? "locator " : "predicate"}  key(${r.scope.map(p => `${r.names[p]}:${r.kind.get(p)}`).join(", ") || "—"})` +
                    `  payload(${r.payload.map(p => r.names[p]).join(", ") || "—"})`);
    const unresolved = [...model.values()].filter(r => r.unresolved);
    console.log(`  JOIN AXIS: ${joinRows.length} multi-clause row(s); a row is one statement, a clause is one`);
    console.log(`             reader call or bare strstr over the document, and its SCOPE is the record key`);
    console.log(`             the model says that reader carries.`);
    console.log(`    NOT MODELLED: ${unresolved.length} reader(s) carry string arguments whose record key this scan could`);
    console.log(`                  not derive${unresolved.length ? ` — ${unresolved.map(r => `${r.name}(${r.calls})`).join(", ")}; every` : ""}`);
    if (unresolved.length)
        console.log(`                  row through them is judged UNSCOPED for a reason about this scan.`);
    console.log(`  FINDINGS`);
    console.log(`    CONTAINED              ${n(cls.contained)}  (reads 1 on a world it never observed)`);
    console.log(`    CROSS-FIELD            ${n(cls.crossField)}  (equals a token spelled as a param NAME)`);
    console.log(`    DECLARED-PREFIX STALE  ${n(cls.declaredStale)}  (the declaration is no longer true)`);
    console.log(`    UNJOINED               ${unjoined.length}  (>=2 clauses tied to no record: one record, or a`);
    console.log(`                              param SWAP, satisfies them all)`);
    console.log(`    DANGLING               ${dangling.length}  (1 unscoped clause beside scoped ones — the scoped`);
    console.log(`                              record answers it, so it restates rather than adds)`);
    console.log(`    REPEATED CLAUSE        ${repeated.length}  (same reader, same key, same payload, twice)`);
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
    const clause = c => `${c.fn ?? "strstr"}(${[...c.scope, ...c.payload].join(", ")})`;
    show("UNJOINED", unjoined, r =>
        `${path}:${r.line}: ${r.unscoped.length} of ${r.n} clause(s) name no record — ` +
        r.unscoped.map(c => `L${c.line} ${clause(c)}`).join("; ") +
        (r.records.length ? ` [the row's scoped clauses are about ${r.records.join(" and ")}]` : ""));
    show("DANGLING", dangling, r =>
        `${path}:${r.line}: ${clause(r.unscoped[0])} at L${r.unscoped[0].line} is unscoped beside ` +
        `${r.scoped.length} clause(s) about ${r.records.join(" and ")} — that record satisfies it`);
    show("REPEATED CLAUSE", repeated, r =>
        `${path}:${r.line}: ` + r.repeated.map(p => `L${p.a.line} and L${p.b.line} both ask ${clause(p.a)}`).join("; "));
    if (CENSUS) {
        show("unscoped but unique today", cls.unique, r => `${path}:${r.line}: ${JSON.stringify(r.needle)}`);
        show("non-literal needles", cls.nonLiteral, r => `${path}:${r.line}: needle composed at runtime`);
    }
    hardTotal += n(cls.contained) + n(cls.crossField) + n(cls.declaredStale) +
                 unjoined.length + dangling.length + repeated.length;
}
/* A zero that means "nothing was asked" must not print like a zero that means "nothing was found" —
 * §MEASURE-WHAT-THE-SHIPPED-PATH-WRITES: an absent count and a zero count are different facts. */
if (unaudited)
    console.log(`\nNOT AUDITED: ${unaudited} of ${FILES.length} file(s) — the scan could not identify the ` +
                `emitted-document variable, so its ${hardTotal} finding(s) are a fact about the OTHER files only.`);
else
    console.log(`\n${hardTotal} finding(s) across TWO axes: whether a probe needle can be answered by a token ` +
                `some OTHER\nstatement of the same fixture emits (CONTAINED, CROSS-FIELD, DECLARED-PREFIX ` +
                `STALE), and whether a\nrow's several clauses are about the SAME record (UNJOINED, DANGLING, ` +
                `REPEATED CLAUSE). They are\nindependent: a row can be clean on the first and assert nothing ` +
                `on the second, which is what\nevery row converted in this fixture so far turned out to be.`);
process.exitCode = hardTotal > 0 || unaudited > 0 ? 1 : 0;
