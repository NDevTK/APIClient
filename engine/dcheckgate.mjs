#!/usr/bin/env node
/* A DCHECK condition that writes. check.h compiles a DCHECK's condition, and a DCHECKF's condition and format
   arguments, to `sizeof` in a release build, so whatever they do happens only in a dev build, and a read made
   inside one leaves release without its value. This audit reads every C source under engine/host (engine/qjs
   excluded, whose headers it reads only for declarations) and reports each DCHECK, DCHECKF, DCHECK_* or
   DCHECKF_* whose condition or arguments hold an assignment, an increment or decrement, or an address handed
   to a parameter its callee may write through, unless the variable is the assert's own scratch. A finding is
   fixed by making the call a statement and asserting its result. Usage: node engine/dcheckgate.mjs [root].
   Exit 0 with no finding, 1 with findings. */
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, relative } from "node:path";

const ROOT = process.argv[2] || join(import.meta.dirname, "host");

function sources(dir, out) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) {
      if (name === "qjs" || name === "out" || name === "node_modules") continue;
      sources(p, out);
    } else if (/\.(c|h)$/.test(name)) {
      out.push(p);
    }
  }
  return out;
}

/* The text with comments, string literals and character literals blanked to spaces, so positions survive and
   nothing inside them is read as code. */
function blank(src) {
  let out = "", i = 0;
  while (i < src.length) {
    const c = src[i], d = src[i + 1];
    if (c === "/" && d === "*") {
      const e = src.indexOf("*/", i + 2), end = e < 0 ? src.length : e + 2;
      out += src.slice(i, end).replace(/[^\n]/g, " ");
      i = end;
    } else if (c === "/" && d === "/") {
      const e = src.indexOf("\n", i), end = e < 0 ? src.length : e;
      out += " ".repeat(end - i);
      i = end;
    } else if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < src.length && src[j] !== c) j += src[j] === "\\" ? 2 : 1;
      out += c + " ".repeat(Math.max(0, j - i - 1)) + c;
      i = j + 1;
    } else {
      out += c;
      i++;
    }
  }
  return out;
}

/* The argument text of the macro call whose open parenthesis is at `open`, up to its balanced close. */
function args(text, open) {
  let depth = 0;
  for (let j = open; j < text.length; j++) {
    if (text[j] === "(") depth++;
    else if (text[j] === ")" && --depth === 0) return text.slice(open + 1, j);
  }
  return null;
}

/* Every function's parameter list, by name, from the definitions and prototypes under ROOT: `params[name]` is
   the list of parameter declarations as written. A name declared twice keeps its first list. */
const params = new Map();
const DECL = /(?:^|[;}\n])\s*(?:static\s+|inline\s+|extern\s+)*[A-Za-z_][\w\s\*]*?\b([A-Za-z_]\w*)\s*\(([^;{}()]*)\)\s*[;{]/g;

/* Whether parameter `i` of `name` is a pointer the callee may write through: a `*` whose pointee is not
   `const`. Undefined when the callee's declaration was not found (a macro, a function pointer, a library). */
function writable(name, i) {
  const list = params.get(name);
  if (list === undefined || i >= list.length) return undefined;
  const p = list[i].replace(/\s+/g, " ").trim(), stars = p.split("*");
  if (stars.length < 2) return false;
  /* The pointee is what the last `*` points at: the text between it and the `*` before it, or the start. */
  return !/\bconst\b/.test(stars[stars.length - 2]);
}

/* The calls in `a`, each with its name and its top-level arguments. */
function calls(a) {
  const out = [];
  const re = /\b([A-Za-z_]\w*)\s*\(/g;
  let m;
  while ((m = re.exec(a)) !== null) {
    let depth = 0, start = m.index + m[0].length, list = [], j;
    for (j = start - 1; j < a.length; j++) {
      if (a[j] === "(") depth++;
      else if (a[j] === ")") { if (--depth === 0) break; }
      else if (a[j] === "," && depth === 1) { list.push(a.slice(start, j)); start = j + 1; }
    }
    list.push(a.slice(start, j));
    out.push({ name: m[1], args: list });
  }
  return out;
}

/* What in `a` writes: an assignment operator, `++`/`--`, or an address handed to a parameter its callee may
   write through. An address handed to a callee whose declaration this audit cannot see is reported too, as
   not decidable here, so the finding is resolved by a statement rather than by a guess. `rest` is the text
   of the enclosing function after this assert with every assert in it blanked: a variable written through an
   address and never named there (a `(void)` of it is no reading) is the assert's own scratch, which release
   need not compute. */
function writes(a, rest) {
  const found = [];
  if (/(^|[^=!<>+\-*/%&|^])=(?!=)/.test(a.replace(/[<>!=]=|==/g, "  "))) found.push("an assignment");
  if (/\+\+|--/.test(a)) found.push("an increment or decrement");
  for (const c of calls(a)) {
    c.args.forEach((arg, i) => {
      const v = /^\s*&\s*([A-Za-z_]\w*)/.exec(arg);
      if (v === null) return;
      const read = new RegExp("\\b" + v[1] + "\\b").test(rest.replace(/\(\s*void\s*\)\s*\w+\s*;/g, ""));
      if (/^\s*&\s*[A-Za-z_]\w*\s*$/.test(arg) && !read) return;
      const w = writable(c.name, i);
      if (w === true) found.push(`\`${arg.trim()}\` handed to ${c.name}'s writable parameter ${i + 1}`);
      else if (w === undefined && !["memcmp", "strcmp", "strncmp", "memchr", "strlen"].includes(c.name))
        found.push(`\`${arg.trim()}\` handed to ${c.name}, whose declaration this audit does not see`);
    });
  }
  return found;
}

const MACRO = /\bDCHECKF?(?:_[A-Z0-9_]+)?\s*\(/g;

/* The text from `at` to the end of the outermost brace block holding it, with each assert's arguments blanked.
   `depth` is the brace depth at `at`. */
function functionRest(text, at, depth) {
  let j;
  for (j = at; j < text.length && depth > 0; j++) depth += text[j] === "{" ? 1 : text[j] === "}" ? -1 : 0;
  let rest = text.slice(at, j), m;
  const re = new RegExp(MACRO.source, "g");
  while ((m = re.exec(rest)) !== null) {
    const inner = args(rest, m.index + m[0].length - 1);
    if (inner === null) break;
    const from = m.index + m[0].length;
    rest = rest.slice(0, from) + " ".repeat(inner.length) + rest.slice(from + inner.length);
  }
  return rest;
}
let findings = 0, sites = 0;
const FILES = sources(ROOT, []);
const QJS = join(ROOT, "..", "qjs");
const DECLARED = FILES.concat(readdirSync(QJS).filter((n) => n.endsWith(".h")).map((n) => join(QJS, n)));
for (const file of DECLARED) {
  const text = blank(readFileSync(file, "utf8"));
  let m;
  DECL.lastIndex = 0;
  while ((m = DECL.exec(text)) !== null) {
    if (["if", "while", "for", "switch", "return", "sizeof"].includes(m[1]) || params.has(m[1])) continue;
    params.set(m[1], m[2].trim() === "void" || m[2].trim() === "" ? [] : m[2].split(","));
    DECL.lastIndex = m.index + m[0].length - 1;
  }
}
for (const file of FILES) {
  const src = readFileSync(file, "utf8"), text = blank(src);
  let m, depth = 0, scanned = 0;
  MACRO.lastIndex = 0;
  while ((m = MACRO.exec(text)) !== null) {
    const before = text.slice(Math.max(0, m.index - 40), m.index);
    if (/#\s*define\s+$/.test(before)) continue;
    const open = m.index + m[0].length - 1, a = args(text, open);
    if (a === null) continue;
    sites++;
    for (; scanned < open + a.length + 2; scanned++) depth += text[scanned] === "{" ? 1 : text[scanned] === "}" ? -1 : 0;
    const w = writes(a, functionRest(text, scanned, depth));
    if (w.length === 0) continue;
    const line = src.slice(0, m.index).split("\n").length;
    console.log(`${relative(process.cwd(), file)}:${line}: ${m[0].replace(/\s*\($/, "")} condition or arguments ` +
                `contain ${w.join(" and ")}, which a release build does not evaluate`);
    findings++;
  }
}
console.log(`[dcheckgate] ${findings} finding(s) over ${sites} DCHECK site(s) under ${relative(process.cwd(), ROOT)}`);
process.exit(findings === 0 ? 0 : 1);
