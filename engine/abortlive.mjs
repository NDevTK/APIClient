// WHETHER AN ABORT THIS TREE ONCE PRODUCED WOULD STILL FIRE, ANSWERED BY CONTENT AND IN THREE VERDICTS.
//
// A census row names an abort by `file:line` plus the `cond` and `reason` check.h wrote. Every reader of such a
// row has had to answer "is this still a defect" by hand, and CLAUDE.md records both halves of how that goes
// wrong. §AND-THE-SIBLING-OF-THAT-IS-A-REPAIR-THAT-WIDENS-AN-ASSERT: a reader whose evidence is that the assert
// is STILL THERE has confused the assert's PRESENCE with the abort's FIRING, and an assert is supposed to be
// present. §AND-THE-CURE-THAT-RECORD-PRESCRIBES-IS-NECESSARY-AND-NOT-SUFFICIENT: a reader who diffs the cond,
// finds it WIDENED and writes GONE has filed a live blocker as fixed, because a widening NARROWS the population
// an assert accuses and never removes it. So this answers PRESENT, ABSENT and CHANGED, and CHANGED is the
// verdict neither hand method has.
//
// WHY THE LINE NUMBER IS IGNORED, WHICH IS NOT A CONVENIENCE BUT THE REASON THIS CANNOT BE A `sed`. A line
// number in a census row DRIFTS the moment anybody adds a paragraph above the assert, and it drifts in BOTH
// directions: a deletion above it moves it UP, so the recorded line still RESOLVES, to code that is real,
// current and about something else. Every check a reader makes then passes. So the question is asked of the
// FILE and keyed on the CONSTRUCT, which is what CLAUDE.md
// §AND-A-PREDICTION'S-FALSIFIER-STATED-AS-A-LINE-NUMBER asks of a falsifier and owes equally to a census row.
//
// WHY COMMENTS ARE STRIPPED FIRST, AND WHY THIS TREE GUARANTEES A RAW GREP ANSWERS WRONGLY. CLAUDE.md
// §AND-VERIFYING-A-REMOVAL-BY-GREPPING-FOR-THE-REMOVED-CONSTRUCT requires a diff that retires a reason to
// REWRITE its site rather than delete it, so after a correct removal the words the assert used are still in the
// file, in a comment, usually in this tree's load-bearing capitals. MEASURED on the two retirements this file
// was written against: `grep -c 'best == 0' engine/host/solver/pending.c` answers 1 at a revision where that
// DCHECK is DELETED (the hit is the paragraph saying it was VACUOUS AND FALSE), and `grep -ci 'container
// queries' engine/host/browser/core/css/css_rule.c` answers 2 where that abort is retired. A text search is
// therefore not a weak instrument here, it is one this tree's own conventions make answer PRESENT for every
// correct repair. What the compiler sees is the only corpus a liveness question may be asked of.
//
// WHY A MESSAGE IS REASSEMBLED RATHER THAN MATCHED WHERE IT LIES. A reason is a C string literal wrapped across
// lines, and `grep` is line-oriented, so a phrase copied out of an emitted record is very often two lines in the
// source and one line in the clipboard — CLAUDE.md §AND-THE-MECHANISM-THAT-DEFEATS-THE-CURE-ABOVE-IS-A-SEARCH-
// STRING-SPANNING-A-LINE-BREAK, whose signature is 0 AT EVERY REVISION INCLUDING ONES WHERE THE TEXT IS THERE.
// This file was written after committing exactly that: a `grep -cliF` for a reason's opening clause answered 0,
// was read as the abort being retired, and the reassembled literal CARRIES that reason's middle sentence
// verbatim under a REWORDED head — which is a CHANGED and was one sentence from being published as an ABSENT.
//
// WHAT THIS DOES NOT ANSWER, NAMED RESIDUAL. WHAT IS NOT COVERED: whether a PRESENT abort would fire on any
// particular page. An assert's presence and its firing are two facts and only a RUN produces the second
// (§AND-THE-CURE-THAT-RECORD-PRESCRIBES); this answers the first and refuses to be read as the second, which is
// why its verdict names the COND rather than the defect. WHAT THE NEXT DIFF BUILDS: a reachability witness
// beside the verdict — the census row, counter or cursor that is nonzero when the assert's own site was
// EXECUTED — so a PRESENT can be read as a live blocker rather than as a live line. HOW ITS ABSENCE WOULD SHOW:
// a reader quoting a PRESENT verdict as a work item for a site whose own run never reached that component.

import { readFileSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

/* A SIGNATURE'S PATH IS REPO-RELATIVE, SO IT IS RESOLVED AGAINST THE REPOSITORY AND NEVER AGAINST THE CALLER'S
   CWD. Every consumer of this is a tool run from wherever its author happened to be standing -- `testing/corpus`
   for the census report -- and a `readFileSync('engine/host/...')` from there throws ENOENT, which this would
   otherwise report as NO-FILE. That is the §THE-VERIFICATION-CAN-FAIL-IN-THE-VERIFIER shape in the direction
   that costs most: a path the caller MIS-ADDRESSED renders identically to a file that has been deleted, and a
   deleted file reads as a retired abort. MEASURED: the first run of this from the census report answered
   `not readable at the working tree` for NINE of its eleven records and the files were all there. The root is
   derived from this module's OWN location -- this file is `<root>/engine/abortlive.mjs` -- so it needs no git,
   no environment and no argument, and it cannot be wrong for a checkout that contains this file at all. */
const REPO_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');

/* check.h's spelling for an UNCONDITIONAL abort. It is the macro's way of saying there was no boolean to test,
   so it says NOTHING about the site and may never answer the cond channel — the same constant report.mjs
   records having printed for five of ten queue entries while the producer had written a cited paragraph into
   `reason` on the same line. A record carrying it is decided on its MESSAGE alone and the verdict says so. */
export const COND_UNCONDITIONAL = 'unreachable';

/* HOW MANY CONSECUTIVE WORDS OF A REASON MAKE A PARTIAL MATCH. A reason is emitted with its `%` conversions
   SUBSTITUTED, so no window spanning one can ever match the literal; the window therefore has to be short
   enough that a substituted reason still has a clean run somewhere in it, and long enough that an ordinary
   English clause shared by two unrelated asserts does not collide. Ten is measured rather than chosen: at ten
   the five real signatures this file was built against separate, and the one that matched partially matched at
   its twenty-ninth word — a sentence the repair kept verbatim under a new head. */
const PARTIAL_WORDS = 10;

const ws = (s) => String(s).replace(/\s+/g, ' ').trim();

/* EVERY ASSERTION MACRO IN THIS TREE, FROM BOTH EMITTERS, AND WHICH ARGUMENT IS THE COND. check.h and
   quickjs-check.h are two headers because the submodule cannot include the host's, and both spell the same six
   shapes; a list short by one makes every abort from that shape read ABSENT, which is the flattering direction.
   The `F` forms take a format string where the plain ones take a message, which changes nothing here: both
   put the message run at the first argument after the cond. */
const MACROS = [
  ['DCHECKF', 1], ['DCHECKF_ONCE', 1], ['DCHECK', 1], ['CHECKF', 1], ['CHECK', 1],
  ['DFAILF', 0], ['DFAIL', 0], ['CHECK_FAILF', 0], ['CHECK_FAIL', 0],
];

/* ONE PASS OVER THE SOURCE: COMMENTS OUT, STRING RUNS JOINED, CHAR LITERALS LEFT ALONE. A `/*` inside a string
   is not a comment and a `"` inside a comment is not a literal, so neither can be found by a regex over the
   whole text — this is a state machine or it is wrong on the first file that holds either, and this tree holds
   both. Adjacent literals separated by nothing but whitespace, a line continuation or a comment are ONE run,
   because that is what C concatenation does and because a wrapped reason is the normal case here. */
function scanSource(src) {
  const code = [];           // what the compiler sees, comments replaced by one space
  const runs = [];           // { text, at } for each concatenated string-literal run
  let run = null, runStart = 0, i = 0;
  const n = src.length;
  const flush = () => { if (run !== null) { runs.push({ text: run, at: runStart }); run = null; } };
  while (i < n) {
    const c = src[i], d = src[i + 1];
    if (c === '/' && d === '*') { const j = src.indexOf('*/', i + 2); i = j === -1 ? n : j + 2; code.push(' '); continue; }
    if (c === '/' && d === '/') { const j = src.indexOf('\n', i); i = j === -1 ? n : j; code.push(' '); continue; }
    if (c === "'") {                                   // char literal: never a string, never a comment opener
      let lit = c; i++;
      while (i < n && src[i] !== "'") { if (src[i] === '\\') { lit += src[i]; i++; } lit += src[i] ?? ''; i++; }
      code.push(lit + "'"); i++; flush(); continue;
    }
    if (c === '"') {
      if (run === null) runStart = i;
      let body = ''; i++;
      while (i < n && src[i] !== '"') {
        if (src[i] === '\\') { body += src[i] + (src[i + 1] ?? ''); i += 2; continue; }
        body += src[i]; i++;
      }
      i++;
      run = (run === null ? '' : run) + body;
      /* THE RUN CONTINUES ONLY IF THE NEXT NON-TRIVIAL CHARACTER IS ANOTHER QUOTE. Whitespace, a backslash
         continuation and a comment all keep a concatenation together in C; anything else ends it, and ending it
         too late is what would weld two different asserts' messages into one false run. */
      let k = i;
      for (;;) {
        while (k < n && /[\s\\]/.test(src[k])) k++;
        if (src[k] === '/' && src[k + 1] === '*') { const j = src.indexOf('*/', k + 2); k = j === -1 ? n : j + 2; continue; }
        if (src[k] === '/' && src[k + 1] === '/') { const j = src.indexOf('\n', k); k = j === -1 ? n : j; continue; }
        break;
      }
      if (src[k] !== '"') flush();
      code.push(' "" ');
      continue;
    }
    code.push(c); i++;
  }
  flush();
  /* THE ESCAPES THAT CHANGE A MATCH, AND ONLY THOSE. `\n` and `\t` become the whitespace the emitted record
     carries; `\"` and `\\` become the character the record carries. Anything else is left as written, because
     inventing a decoding for an escape this tree does not use would make a literal match text it is not. */
  const unesc = (s) => s.replace(/\\n/g, ' ').replace(/\\t/g, ' ').replace(/\\"/g, '"').replace(/\\\\/g, '\\');
  return { code: ws(code.join('')), runs: runs.map((r) => ({ text: ws(unesc(r.text)), at: r.at })), raw: src };
}

/* THE COND AS THE PREPROCESSOR STRINGIFIED IT, TAKEN BY BALANCED-PAREN SCAN AND NEVER BY A COMMA SPLIT. A cond
   is an expression and holds commas of its own (`f(a, b) == 0`), so splitting on the first comma truncates
   exactly the conds most worth comparing. The scan walks depth and skips the string runs the pass above has
   already found, so a comma inside a message cannot end an argument either. */
function macroCalls(src) {
  const out = [];
  for (const [name, condArg] of MACROS) {
    const re = new RegExp('(?:^|[^A-Za-z0-9_])' + name + '\\s*\\(', 'g');
    let m;
    while ((m = re.exec(src)) !== null) {
      let i = m.index + m[0].length, depth = 1, arg = '', args = [];
      while (i < src.length && depth > 0) {
        const c = src[i];
        if (c === '"' || c === "'") {                  // skip a literal whole; it cannot close a paren
          const q = c; arg += c; i++;
          while (i < src.length && src[i] !== q) { if (src[i] === '\\') { arg += src[i]; i++; } arg += src[i]; i++; }
          arg += q; i++; continue;
        }
        if (c === '/' && src[i + 1] === '*') { const j = src.indexOf('*/', i + 2); i = j === -1 ? src.length : j + 2; arg += ' '; continue; }
        if (c === '(') depth++;
        if (c === ')') { depth--; if (depth === 0) { args.push(arg); break; } }
        if (c === ',' && depth === 1) { args.push(arg); arg = ''; i++; continue; }
        arg += c; i++;
      }
      out.push({ name, at: m.index, condText: condArg === 0 ? COND_UNCONDITIONAL : ws(args[0] ?? ''), args });
    }
  }
  return out;
}

/* WHETHER A LITERAL IS THE REASON THAT WAS EMITTED, UP TO ITS SUBSTITUTIONS. A `%` conversion is replaced before
   the record is written, so the literal and the record can never be string-equal for a `DCHECKF`; what is
   comparable is the FIXED SEGMENTS between conversions, all of them, IN ORDER. A reworded message fails that
   and a substituted one passes it, which is the whole distinction the CHANGED verdict rests on.
   AND CONTAINMENT IN ORDER IS NOT ENOUGH, WHICH THIS FILE'S OWN RUN AGAINST THE REAL TREE ESTABLISHED. The test
   is between a SHORT literal and a LONG reason, so a containment check is satisfied by any small literal whose
   words happen to occur in the reason — measured: a retired container-query DFAIL answered PRESENT "segment for
   segment" while its distinctive clause occurs in NO literal of that file at all, because some other short
   literal of the same component was found scattered through the reason's five hundred characters. A verdict
   reached that way is the flattering one in the direction that matters: it reads a retired abort as live.
   SO THE LITERAL MUST ACCOUNT FOR THE REASON RATHER THAN MERELY BE FOUND INSIDE IT, and two conditions do it.
   The literal's fixed text must COVER most of the reason, since substitution can only ever add a few characters
   per conversion; and the reason must BEGIN where the literal begins, because the emitted record IS the literal
   with its conversions replaced — unless the literal opens with a conversion, which is the one case where the
   record's first characters are the substituted value and no prefix is owed. */
const REASON_COVERAGE = 0.6;
function literalIsReason(literal, reason) {
  const lit = ws(literal);
  const conv = /%[-+ #0-9.*']*(?:hh|h|ll|l|L|z|j|t)?[diouxXeEfFgGaAcspn%]/;
  const segs = lit.split(new RegExp(conv.source, 'g')).map(ws).filter((s) => s.length >= 4);
  if (!segs.length || !reason) return false;
  let at = 0, covered = 0;
  for (const s of segs) {
    const k = reason.indexOf(s, at);
    if (k === -1) return false;
    if (covered === 0 && k !== 0 && !conv.test(lit.slice(0, 2))) return false;   // the reason starts where the literal does
    at = k + s.length;
    covered += s.length;
  }
  return covered >= REASON_COVERAGE * ws(reason).length;
}

function partialWindow(reason, runs) {
  const w = ws(reason).split(' ').filter(Boolean);
  const hay = runs.map((r) => r.text).join('\n');
  if (w.length < PARTIAL_WORDS) return hay.includes(w.join(' ')) ? 0 : -1;
  for (let i = 0; i + PARTIAL_WORDS <= w.length; i++)
    if (hay.includes(w.slice(i, i + PARTIAL_WORDS).join(' '))) return i;
  return -1;
}

export const VERDICT_PRESENT = 'PRESENT';   // this cond and this message both still compile in, unchanged
export const VERDICT_CHANGED = 'CHANGED';   // the site stands and the assert is not the one that fired
export const VERDICT_ABSENT  = 'ABSENT';    // neither the cond nor any run of the message is in the file
export const VERDICT_NO_FILE = 'NO-FILE';   // the path the record names is not in the tree at that revision
export const VERDICT_UNKEYED = 'NOT-KEYED-ON-A-FILE';   // e.g. the js mirror, which emits no file:line

/* THE TRUSTED ZONE'S OWN `.js`, DERIVED FROM WHAT GIT TRACKS AND NEVER A LIST HERE. The js mirror's records are
   the ONE band this file could not grade, and they are not a small band: measured over one corpus, the js-side
   key ranked FIRST by sites hit with SIX distinct operands under it, every one of them reading `cannot ask`.
   A #1 queue entry nothing can grade is the §A-VERDICT-THAT-IS-RED-ON-EVERY-RUN furniture, with the twist that
   it is not red — it is UNREADABLE, which a reader skips for the same reason. */
const zoneFiles = (rev) => {
  const out = execFileSync('git', rev ? ['ls-tree', '-r', '--name-only', rev, '--', 'extension']
                                     : ['ls-files', '--', 'extension'],
                           { cwd: REPO_ROOT, encoding: 'utf8', maxBuffer: 1 << 28 });
  return out.split('\n').filter((f) => /\.js$/.test(f));
};

/* THE MESSAGE ASKED OF THE WHOLE ZONE, BECAUSE THE FILE THE RECORD NAMES IS THE WRONG FILE BY CONSTRUCTION.
   `extension/check.js` is where the macro is DEFINED; the message is written at the CALLER, in whichever of the
   zone's files holds it. Keying on the named file would answer ABSENT for every js record and retire the whole
   band — which is the false ABSENT this file exists to refuse, so the population is widened rather than the
   verdict weakened.
   IT IS A MESSAGE-ONLY VERDICT AND SAYS SO, which is strictly weaker than the C arm's. The js mirror throws an
   Error and emits no cond, so there is no predicate to compare and the message is the whole of the identity --
   exactly the `COND_UNCONDITIONAL` case above, and the same three outcomes follow from the same two tests.
   WHAT IT RESTS ON IS MEASURED AND NOT ASSUMED: `scanSource` reads DOUBLE-quoted runs, and over the zone's abort
   calls the message opens with `"` 550 times, with `'` ZERO times and with a backtick ZERO times. A single-quoted
   message would be consumed as a C char literal and produce no run, so the convention is the precondition --
   and it is a precondition about the ZONE, which is why it is stated as a count and not as a promise.
   AND IT NAMES THE FILE THAT HOLDS THE MESSAGE, which is the second half of what the band cost: a key on the
   macro's own file buckets unrelated aborts under one entry, so the ranking's order is wrong and its count is
   inflated. The holding file is the identity the record never carried. */
function zoneVerdict(reason, rev) {
  const want = ws(reason || '');
  if (!want) return { verdict: VERDICT_UNKEYED, file: ZONE_LABEL,
                      why: 'this record carries no reason text, so there is no message to ask the zone for' };
  const full = [], part = [];
  for (const f of zoneFiles(rev)) {
    let s;
    try {
      s = scanSource(rev ? execFileSync('git', ['show', `${rev}:${f}`],
                                        { cwd: REPO_ROOT, encoding: 'utf8', maxBuffer: 1 << 28 })
                         : readFileSync(resolve(REPO_ROOT, f), 'utf8'));
    } catch { continue; }
    if (s.runs.some((r) => literalIsReason(r.text, want))) { full.push(f); continue; }
    if (partialWindow(want, s.runs) >= 0) part.push(f);
  }
  if (full.length)
    return { verdict: VERDICT_PRESENT, file: full.join(' '), messageSeen: true,
             why: 'the message still compiles in, segment for segment, at ' + full.join(' ') + ' — a MESSAGE-ONLY ' +
                  'verdict over the trusted zone, because the js mirror emits no cond and there is no predicate ' +
                  'to compare' };
  if (part.length)
    return { verdict: VERDICT_CHANGED, file: part.join(' '), messageSeen: false,
             why: 'only part of the message survives, at ' + part.join(' ') + ' — the refusal was REWORDED, so ' +
                  'the record and the site are two different asserts' };
  return { verdict: VERDICT_ABSENT, file: ZONE_LABEL, messageSeen: false,
           why: 'no run of this message is a string literal of ANY tracked `.js` in the trusted zone' };
}
const ZONE_LABEL = 'extension/**/*.js (the trusted zone, searched whole)';

/* THE VERDICT FOR ONE `@WHY`. `at` may carry its drifted `:line` and it is DISCARDED; `rev` omitted reads the
   working tree, which is the revision a reader is standing in. */
export function condVerdict({ at, cond, reason, zone }, rev) {
  const file = String(at || '').replace(/:\d+$/, '').trim();
  /* A RECORD THAT NAMES ITS CORPUS IS ROUTED THERE, AND IT SAYS SO IN A FIELD RATHER THAN IN THE SHAPE OF A
     LABEL. The js mirror throws an Error and carries no file:line, so its producer records `at: ''` on purpose —
     and an empty path is indistinguishable from a malformed one, which is why answering `cannot ask` put six
     unrelated aborts in one ungradeable bucket at the TOP of a work queue. `zone: true` is the producer saying
     WHICH corpus holds the message, which is a fact it has and this file does not. It is a FLAG and never a
     parse of `at`, because sniffing `extension/check.js (js side)` would key on the file the macro is DEFINED
     in — the exact false-ABSENT the js band's own residual forbids. */
  if (zone) return zoneVerdict(reason, rev);
  if (!file || /\s/.test(file) || !/\.(c|h|mjs|js)$/.test(file))
    return { verdict: VERDICT_UNKEYED, file,
             why: 'this record carries no source file, so there is no construct to ask for — the js mirror ' +
                  'throws an Error and emits no file:line, and the file its own macro is DEFINED in is not the ' +
                  'file its message is written in' };
  let src;
  try {
    src = rev ? execFileSync('git', ['show', `${rev}:${file}`],
                             { cwd: REPO_ROOT, encoding: 'utf8', maxBuffer: 1 << 28 })
              : readFileSync(resolve(REPO_ROOT, file), 'utf8');
  } catch { return { verdict: VERDICT_NO_FILE, file, why: `${file} is not readable at ${rev || 'the working tree'}` }; }

  const s = scanSource(src);
  /* THE CALLS ARE READ OUT OF THE COMMENT-STRIPPED CODE AND NEVER OUT OF THE RAW TEXT, and this file's own
     control is what established it: run over the raw source, the argument scan FOUND `DCHECK(best == 0, …)`
     inside the very paragraph recording that that assert was DELETED, and the retired case answered CHANGED
     instead of ABSENT. The defect this checker exists to refuse, committed inside it, caught because the
     control had to speak before any verdict could be published. The stripped text replaces each literal with
     an empty one, which costs the cond channel nothing: a cond is an expression and never a string. */
  const calls = macroCalls(s.code);
  const condWanted = ws(cond || '');
  const reasonWanted = ws(reason || '');
  const condSeen = condWanted && condWanted !== COND_UNCONDITIONAL &&
                   calls.some((c) => c.condText === condWanted);
  /* A MESSAGE MATCH IS ASKED OF THE REASSEMBLED RUNS AND NOT OF A MACRO ARGUMENT, because a reason is sometimes
     a named constant or a run assembled from several literals one of which another macro supplies — and
     because the quickjs emitter's convention is `DCHECK(expr, "expr")`, where the record's reason IS the
     stringified expression and lives in the file as an ordinary literal. */
  const full = s.runs.some((r) => literalIsReason(r.text, reasonWanted));
  const part = full ? 0 : partialWindow(reasonWanted, s.runs);
  const condOnly = condSeen && !full;

  let verdict, why;
  if (condWanted === COND_UNCONDITIONAL || !condWanted) {
    /* AN UNCONDITIONAL ABORT HAS NO PREDICATE TO COMPARE, SO ITS MESSAGE IS THE WHOLE OF ITS IDENTITY. A
       reworded reason is a different refusal — measured: one site's repair kept a middle sentence verbatim,
       renamed the carrier in its head, and said in its own new text that the population the old record named
       is now BUILT. Reading that as PRESENT dispatches a lane at work somebody finished. */
    verdict = full ? VERDICT_PRESENT : part >= 0 ? VERDICT_CHANGED : VERDICT_ABSENT;
    why = full ? 'the message still compiles in, segment for segment'
        : part >= 0 ? `only part of the message survives (from its word ${part}) — the refusal was REWORDED, so the record and the site are two different asserts`
        : 'no run of this message is a string literal of this file';
  } else if (condSeen && full) {
    verdict = VERDICT_PRESENT; why = 'this cond and this message are both still written at this site';
  } else if (condOnly || (full && !condSeen)) {
    verdict = VERDICT_CHANGED;
    why = condOnly ? 'the cond is still written and its message is not — the predicate stands under a different refusal'
                   : 'the message is still written and this cond is not — the predicate was WIDENED or replaced, which narrows the population it accuses and does not remove it';
  } else if (part >= 0) {
    verdict = VERDICT_CHANGED; why = `neither the cond nor the whole message, but part of the message survives (from its word ${part})`;
  } else {
    verdict = VERDICT_ABSENT; why = 'neither this cond nor any run of this message is in the file';
  }
  return { verdict, file, why, condSeen, messageSeen: full, partialFrom: part, calls: calls.length, runs: s.runs.length };
}

/* THE CONTROL, AND IT RUNS BEFORE ANY VERDICT IS PUBLISHED. CLAUDE.md §AND-THE-WAY-YOU-ESTABLISH-WHICH-HALF-A-
   FAILURE-LANDS-IN: a probe that has never been shown REJECTING something has calibrated nothing, and the
   direction that matters here is ABSENT — a false ABSENT retires a live blocker and nothing downstream
   contradicts it. The fixtures are SYNTHETIC and inline rather than taken from this tree, because a control
   keyed on a real file's content is a coordinate that rots: the day somebody repairs that file the control
   fails and reads as this checker breaking. Each case states which verdict it must produce and WHY it is the
   case that cannot be got right by accident. */
const SELFTEST = [
  { name: 'present: cond and message both written',
    src: 'void f(void){ DCHECK(a == 1, "the widget list is empty and the walk above says it cannot be"); }',
    why: { cond: 'a == 1', reason: 'the widget list is empty and the walk above says it cannot be' },
    want: VERDICT_PRESENT },
  { name: 'absent: the retired argument stays in a comment (a raw grep says PRESENT)',
    src: '/* AND THE ASSERT THAT STOOD HERE WAS VACUOUS: it was `DCHECK(best == 0, …)` under a message about\n' +
         '   the register not being in arrival order, and it is DELETED rather than repaired. */\nvoid f(void){ g(); }',
    why: { cond: 'best == 0', reason: 'this reply register\'s deliverable entries are not in arrival order' },
    want: VERDICT_ABSENT },
  { name: 'changed: the cond was WIDENED and is not a substring question',
    src: 'void f(void){ DCHECKF(refcount(x) == 2 + own + base, "held by %d where the holders this line can '
       + 'ACCOUNT FOR are %d", refcount(x), 2 + own + base); }',
    why: { cond: 'refcount(x) == 2', reason: 'held by 7 where the holders this line can ACCOUNT FOR are 4' },
    want: VERDICT_CHANGED },
  { name: 'present: a DCHECKF whose message is only comparable up to its conversions',
    src: 'void f(void){ DCHECKF(refcount(x) == 2 + own + base, "held by %d where the holders this line can '
       + 'ACCOUNT FOR are %d", refcount(x), 2 + own + base); }',
    why: { cond: 'refcount(x) == 2 + own + base', reason: 'held by 7 where the holders this line can ACCOUNT FOR are 4' },
    want: VERDICT_PRESENT },
  { name: 'present: a message WRAPPED across lines, which a line-oriented grep cannot find',
    src: 'void f(void){\n    DFAILF("a document name resolved to no navigable of THIS TIMELINE at all, asked "\n'
       + '           "at %s:%d — it names a document no timeline of this instance has opened", file, line);\n}',
    why: { cond: COND_UNCONDITIONAL,
           reason: 'a document name resolved to no navigable of THIS TIMELINE at all, asked at x.c:1 — it names '
                 + 'a document no timeline of this instance has opened' },
    want: VERDICT_PRESENT },
  { name: 'changed: an unconditional refusal REWORDED, keeping one sentence verbatim',
    src: 'void f(void){ DFAIL("HTML §2.7: a CONCOLIC reached the serializer on a path whose CARRIER is one '
       + 'ArrayBuffer. A live value crosses neither a park, a session nor an instance, so the arm is the triple '
       + 'written AS DATA, which is what the queued delivery now carries"); }',
    why: { cond: COND_UNCONDITIONAL,
           reason: 'HTML §2.7: a CONCOLIC reached the serializer on the path that hands BYTES to a LATER turn — '
                 + 'a queued port or window delivery, a history entry, a broadcast. A live value crosses neither '
                 + 'a park, a session nor an instance, so this arm is the triple written AS DATA' },
    want: VERDICT_CHANGED },
  { name: 'absent: a comment that opens inside a string must not end the literal',
    src: 'void f(void){ DFAIL("a path like /* this one */ is text and not a comment opener"); }',
    why: { cond: COND_UNCONDITIONAL, reason: 'some refusal this file has never written down at all' },
    want: VERDICT_ABSENT },
  { name: 'present: a cond carrying a comma of its own survives the argument scan',
    src: 'void f(void){ DCHECK(cmp(a, b) == 0, "the two reads walk one table in one applied delta"); }',
    why: { cond: 'cmp(a, b) == 0', reason: 'the two reads walk one table in one applied delta' },
    want: VERDICT_PRESENT },
];

/* THE SELF-TEST READS A SOURCE STRING AND NOT A PATH, so `condVerdict`'s file read is the one thing it cannot
   exercise — which is why the real-tree read is exercised by the CLI and by report.mjs, and why this function
   takes the scan and the match rather than the whole entry. */
export function selftest() {
  const fails = [];
  for (const t of SELFTEST) {
    const s = scanSource(t.src);
    const calls = macroCalls(scanSource(t.src).code);
    const condWanted = ws(t.why.cond || '');
    const reasonWanted = ws(t.why.reason || '');
    const condSeen = condWanted && condWanted !== COND_UNCONDITIONAL && calls.some((c) => c.condText === condWanted);
    const full = s.runs.some((r) => literalIsReason(r.text, reasonWanted));
    const part = full ? 0 : partialWindow(reasonWanted, s.runs);
    let got;
    if (condWanted === COND_UNCONDITIONAL || !condWanted) got = full ? VERDICT_PRESENT : part >= 0 ? VERDICT_CHANGED : VERDICT_ABSENT;
    else if (condSeen && full) got = VERDICT_PRESENT;
    else if (condSeen || full || part >= 0) got = VERDICT_CHANGED;
    else got = VERDICT_ABSENT;
    if (got !== t.want) fails.push(`${t.name}: wanted ${t.want} got ${got} (cond=${condSeen} msg=${full} part=${part})`);
  }
  return fails;
}

/* ARMED OR NOTHING IS PUBLISHED. A checker whose own control has not spoken is a checker reporting on its own
   probe, and the cheap failure here is silent: every record would read ABSENT and every retired-looking row
   would be a row somebody stops working on. */
/* THE ZONE ARM'S OWN SEPARATING PAIR, OVER REAL ZONE CONTENT AND NOT OVER A SYNTHETIC SOURCE. The synthetic
   cases above cannot arm it, because its whole subject is the TREE: what it has to be shown doing is answering
   PRESENT for a message the zone really holds and ABSENT for one it does not. The PRESENT half is taken from the
   zone's own scan at run time rather than hand-picked, so it cannot go stale and cannot be a message somebody
   retired; the ABSENT half is invented and can never be there. Two verdicts from one code path over one
   population is what `armed` means -- §A-CONTROL-ARMS-ONLY-ON-A-SITE's two runs, and the first of them has to
   SPEAK before the second is worth anything. */
export function zoneSelftest() {
  const fails = [];
  let sample = null;
  for (const f of zoneFiles('')) {
    const s = scanSource(readFileSync(resolve(REPO_ROOT, f), 'utf8'));
    const r = s.runs.find((x) => ws(x.text).split(' ').length >= PARTIAL_WORDS + 4 && !/%/.test(x.text));
    if (r) { sample = ws(r.text); break; }
  }
  if (!sample) fails.push('zone: no run long enough to use as a PRESENT control was found in any zone `.js` — ' +
                          'the arm cannot be armed, so its verdicts may not be believed');
  else {
    const got = zoneVerdict(sample, '').verdict;
    if (got !== VERDICT_PRESENT) fails.push(`zone PRESENT control: wanted ${VERDICT_PRESENT} got ${got}`);
  }
  const absent = zoneVerdict('zzqq no run of this sentence is written in any file of this trusted zone at all ' +
                             'and it never will be because it was invented for a control', '').verdict;
  if (absent !== VERDICT_ABSENT) fails.push(`zone ABSENT control: wanted ${VERDICT_ABSENT} got ${absent}`);
  return fails;
}

export function armed() {
  const fails = selftest().concat(zoneSelftest());
  if (fails.length)
    throw new Error('engine/abortlive.mjs: its own control does not separate the verdicts, so no verdict it\n' +
      '  produces may be believed — a false ABSENT retires a live blocker and nothing downstream contradicts it:\n' +
      fails.map((f) => '    ' + f).join('\n'));
  return SELFTEST.length;
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const n = armed();
  const args = process.argv.slice(2);
  if (args[0] === '--selftest' || !args.length) {
    console.log(`abortlive: control armed, ${n} synthetic cases separate PRESENT / CHANGED / ABSENT.`);
    console.log('usage: node engine/abortlive.mjs [<revision>] < one-@WHY-record-per-line');
    console.log('       a record is {"at":"file:line","cond":"…","reason":"…"} or a raw check.h @WHY line.');
    if (args[0] === '--selftest') process.exit(0);
    process.exit(0);
  }
  const rev = args[0] === '-' ? '' : args[0];
  const text = readFileSync(0, 'utf8');
  for (const line of text.split('\n')) {
    const t = line.trim();
    if (!t) continue;
    let rec;
    try { rec = JSON.parse(t); } catch { rec = null; }
    if (!rec || !rec.at) {
      const m = /"phase":"assert","cond":"([\s\S]*?)","at":"([^"]*)","reason":"([\s\S]*?)"\}/.exec(t);
      if (!m) { console.log(`?  could not read a record out of: ${t.slice(0, 120)}`); continue; }
      rec = { cond: m[1], at: m[2], reason: m[3].replace(/\\n/g, ' ').replace(/\\"/g, '"') };
    }
    const v = condVerdict(rec, rev);
    console.log(`${v.verdict.padEnd(20)} ${rec.at}\n     ${v.why}`);
  }
}
