/* WHAT ENCLOSES A USE OF A PLATFORM NAME — TAKEN FROM A REAL PARSE, ONE OCCURRENCE AT A TIME.
 *
 * engine/absentrank.mjs bands an absent global by what its absence COSTS: THROWS (no channel saw a
 * non-throwing read of this name ANYWHERE), `mixed`, `detect-only`. That band is stated over the WHOLE
 * CORPUS, and its own header says why that is as far as a channel can go — "a channel matches one
 * EXPRESSION and a `try` block is a SCOPE, so whether an occurrence is inside one is a question about what
 * ENCLOSES the occurrence". This file asks that question. It is the operand its NAMED RESIDUAL asks for and
 * the thing its RETIREMENT names: "when a THROWS row's own sites are tested for these shapes by the
 * instrument rather than by its reader, at which point the band states a per-site fact".
 *
 * WHY A PER-SITE ANSWER IS WORTH A PARSE, AND IT IS NOT THE THROWS BAND THAT NEEDS IT. A THROWS row's whole
 * count IS its number of sites and those rows stand at one to three, so opening them by hand is cheaper than
 * building an interface — absentrank says so and is right. The band that cannot be dispatched from is
 * `mixed`, whose rows carry tens of occurrences and whose own definition is "some use and some guard exist
 * and only reading the site says which covers which". A reader will not open forty sites, so a `mixed` row
 * is read as hedged when it may be a flow-ender at almost every one of them — and `mixed` sorts BELOW every
 * THROWS row, so the queue's head is decided by a band that cannot see the difference.
 *
 * THE FOUR VERDICTS ARE FOUR OUTCOMES OF THE SAME ABSENCE AND THEY TAKE DIFFERENT WORK, WHICH IS THE ONLY
 * REASON TO SPLIT THEM. With the global absent, at an occurrence that EVALUATES the binding:
 *   throws           nothing covers it — ReferenceError, and the flow ends on that line. This is the cost
 *                    §NO-STUBS is about and the only one of the four that ends a flow.
 *   caught           the occurrence is inside the `block` of a `try` that HAS a handler. The reference still
 *                    raises; the catch arm runs and the page goes on doing what it would do in a real
 *                    browser lacking the name. That is the detect-only verdict reached by another route.
 *   guarded-fallback a presence test of THIS name selects the arm holding the occurrence, and the construct
 *                    has an other arm. The use is never reached, and something else runs instead.
 *   guarded-silent   the same, with NO other arm. The use is never reached and NOTHING runs instead — which
 *                    is the loss §NO-STUBS names: the guard answers false exactly as it would in a browser
 *                    without the name, so no crash, no fork and no signal, while every endpoint and every
 *                    sink behind that branch goes unreached with nothing anywhere saying so.
 * A `guarded-silent` row is therefore not a cheap row. It is the expensive one that nothing else can see.
 *
 * ITS ERRORS LAND IN THE PROMOTING DIRECTION ON PURPOSE, AND THAT IS A DESIGN CHOICE WITH A REASON RATHER
 * THAN A DEFAULT. absentrank records that a GUARD widening "can only ever DEMOTE, so its errors land in the
 * silencing direction, where an under-claim is not found by acting on it", and refuses one that traded one
 * true demotion for two false ones. Every verdict here but `throws` is a demotion, so ANYTHING THIS FILE
 * CANNOT ESTABLISH ANSWERS `throws`: an unrecognised test shape, a receiver that is not one of the three
 * global spellings, a name that is not the one being tested, an operator whose polarity does not decide,
 * and every node type not enumerated below. The cost of that bias is an over-stated `throws` count, which
 * sends a reader to OPEN a site; the cost of the other bias is a row that silently leaves the queue.
 *
 * POLARITY IS THE WHOLE OF THE DIFFICULTY AND A NAIVE READER GETS IT BACKWARDS IN THE SILENCING DIRECTION.
 * "The occurrence sits inside an `if` whose test mentions the name" is NOT a guard: `if (typeof X ===
 * "undefined") { new X(1) }` puts the use in the arm taken when the name is ABSENT, so it throws, and a
 * reader that demoted it would have removed a real flow-ender. Two more shapes invert the same way and both
 * are armed below: the right arm of `||` runs when the left is FALSY, so `typeof X !== "undefined" || new
 * X(1)` throws while `typeof X === "undefined" || new X(1)` does not; and a bare `if (X)` is not a guard at
 * all, because THE TEST ITSELF evaluates the binding and raises before any arm is entered. So the question
 * asked of every enclosing construct is not "does this test mention the name" but "does this test being
 * TRUE entail that the name is defined", answered as +1/-1/0, with 0 meaning `throws`.
 *
 * IT IS LEXICAL CONTAINMENT AND NOT CONTROL-FLOW DOMINANCE, WHICH IS STATED BECAUSE THE WORD `dominates`
 * WOULD BE AN OVER-CLAIM AND ONE LINE REFUTES IT. `if (typeof X !== "undefined") {} new X(1)` really is
 * dominated by a successful test and really does throw, and this file answers `throws` for it — correctly,
 * and by the narrower rule rather than by the one the word would promise. What is covered is an occurrence
 * INSIDE the selected arm, which is how feature detection is actually written; a guard whose true branch
 * reaches the use through a later statement is a FLOOR and is named in the residual below.
 *
 * THE INNERMOST DECIDING ANCESTOR WINS, so the verdict is deterministic when several qualify. `try { if
 * (typeof X !== "undefined") { new X(1) } }` reads `guarded-silent` and `if (typeof X !== "undefined") { try
 * { new X(1) } catch {} }` reads `caught`; both say the flow does not end, and which word a reader gets is
 * decided by position rather than by a precedence rule nobody could check.
 *
 * ITS MEASURED PRICE, held to the standard engine/absentrank.mjs holds its own widenings to. It moved NO
 * count, NO class, NO rank and NO row in its caller: the column is a pure addition beside the existing ones.
 * What it BUYS is that the caller's cost order and the flow-ending order DISAGREE AT THE HEAD — measured on
 * the corpus it landed against, the largest flow-ending row IN the THROWS band stood at THREE sites while
 * four `mixed` rows below it carried 48, 42, 40 and 27, and TWO of the SIX THROWS rows carried no
 * flow-ending site at all. The DEMOTING verdicts were opened and read before the column was believed,
 * because they are the ones whose error removes a row from a queue rather than adding one to it.
 *
 * WHAT IT DOES NOT COVER, stated because a caller printing these counts needs to know what they still hold:
 *   - A FILE THE PARSER REFUSES is reported as `parsed:false` with the parser's own message, and NOTHING is
 *     claimed about its occurrences. Same treatment for a walk that fails on its own depth — reported under
 *     its own reason rather than merged into a verdict, because a refusal counted as `throws` would be an
 *     accusation this file cannot support and one counted as guarded would be a silent demotion.
 *   - AN OFFSET THAT DOES NOT LAND ON AN IDENTIFIER OF THAT NAME gets no verdict at all, and the caller sees
 *     it as unlocated. That is every occurrence its channel matched inside a string, a template, a comment
 *     or a property key. IT IS NOT A SECOND COPY OF engine/js_code_refs.mjs's QUESTION AND MUST NOT BE USED
 *     AS ONE: that reader decides the caller's `notcode` subtraction and this decides nothing, so a
 *     disagreement between them is a finding rather than a tie to be broken. They are derived from two
 *     different parsers by two different mechanisms, which is what makes their agreement worth reading and
 *     their difference worth opening.
 *   - A GUARD ON A DIFFERENT NAME THAT NEVERTHELESS COVERS THIS ONE — a sibling capability's absence making
 *     the branch dead — is absentrank's own standing residual and is untouched here.
 *
 * NAMED RESIDUAL — A GUARD THAT DOES NOT LEXICALLY CONTAIN ITS USE IS NOT SEEN. WHAT IS NOT COVERED: this
 * file decides an occurrence by what ENCLOSES it, so a test that selects the use by TERMINATING the
 * alternative — `if (typeof X === "undefined") return;` followed by a use later in the same body — protects
 * the use exactly as an `if` wrapping it would, and reads `throws`. The property, not the population: the
 * question this file asks is containment and the question that shape poses is REACHABILITY, and no widening
 * of an ancestor walk answers a different question. WHAT THE NEXT DIFF BUILDS: for a `throws` occurrence,
 * whether any statement PRECEDING it in an enclosing body is a presence test of the same name whose taken arm
 * terminates — which is a walk over sibling statements rather than over ancestors, and is the same operand
 * absentrank's own sibling-capability residual asks for one name over. HOW ITS ABSENCE WOULD SHOW: a row
 * whose `thr` is large while opening its sites finds the reads sitting under a function whose first
 * statement tests the same name and returns. ITS SIZE IS MEASURED RATHER THAN GUESSED, because a residual
 * that cannot say whether its population is empty is a reason to build nothing: over the corpus this landed
 * against, ZERO of the `throws` sites sat behind such a guard, with the probe armed both ways — a synthetic
 * early return fires it, a non-terminating consequent and a test of a DIFFERENT name do not. Empty TODAY and
 * not by construction, which is why the residual stands rather than being discharged.
 *
 * NAMED RESIDUAL — A GUARD SPELLED ON A BOUND GLOBAL ALIAS IS NOT A GUARD HERE. WHAT IS NOT COVERED: the
 * three receivers above are matched LITERALLY, so a file that binds the global object to a local name and
 * tests the capability through it reads `throws`. WHAT THE NEXT DIFF BUILDS: nothing, until a scope-aware
 * reader exists — absentrank BUILT this widening, MEASURED it and DECLINED it, because the aliases a minified
 * bundle binds are its ordinary one-letter locals and a per-file alias set attributes an inner scope's
 * parameter to the global; its measurement stands and this file inherits both the gap and the refusal. HOW
 * ITS ABSENCE WOULD SHOW: a row whose `thr` is large while opening its sites finds the reads on a
 * one-letter receiver that some earlier line of the same file assigned the global object to.
 *
 * A SPELLING THIS READER CANNOT SEE IS NOT A SMALLER TABLE, IT IS A FLOW-ENDER REPORTED WHERE THE PAGE
 * HANDLES THE ABSENCE — AND THE BIAS ABOVE IS WHAT HIDES IT. The promoting bias makes an unrecognised test
 * answer `throws`, which is the right default and is also why a missing SPELLING costs nothing visible: the
 * row simply reads as more flow-ending than it is, in the one column a reader dispatches from, with the
 * output identical to a site that really is unguarded. So the question "which spellings does this reader
 * know" is not a tidy-up — it is the only thing standing between the promoting bias and a column that means
 * what it says.
 * TWO WERE MISSING AND BOTH ARE A MINIFIER'S ORDINARY OUTPUT RATHER THAN AN EXOTIC AUTHOR. The relational
 * `typeof X<"u"` is what esbuild and terser emit for `typeof X !== "undefined"`, and a substitution-free
 * TEMPLATE literal is what several bundles here emit for every string they contain. Their size was measured
 * over the corpus rather than assumed, and the derivation is the command rather than the figure, because a
 * corpus moves:
 *     grep -rohE 'typeof +[A-Za-z_$][A-Za-z0-9_$]* *[<>]=? *("u"|`u`)' <corpus> | wc -l
 * When it was taken it read 1707 occurrences over all identifiers, of which 469 carry the BACKTICK spelling,
 * plus 399 written the other way round as `"u" > typeof X`; `<=` and `>=` read ZERO, which is why they are
 * refused above rather than admitted on the same argument.
 * ITS MEASURED PRICE, held to the standard this file's own landing was held to. It moves NO count, NO class
 * and NO rank in the caller, which is a STRUCTURAL fact rather than a run: absentrank reads this reader's
 * verdicts into `guardOf` and `guardOf` is consumed by the printed split columns and by the aggregate line
 * and by nothing else, so no channel total, no band and no sort can depend on it. A before/after over one
 * frozen corpus confirms it — every line of the caller's output identical except the five split columns of
 * eight rows. What it MOVES is 22 of 318 use sites, ALL of them from a costlier verdict to a cheaper one and
 * NONE in the other direction: 11 `throws` to `guarded-silent`, 6 `throws` to `guarded-fallback`, 4 `caught`
 * to `guarded-fallback` (a guard INSIDE the try, which the innermost-ancestor rule then decides), and the
 * aggregate goes 237/25/2/20/34 to 219/21/12/32/34. EVERY ONE OF THE 22 WAS OPENED AND READS AS ITS NEW
 * VERDICT — they are `typeof ClipboardEvent<`u`?new ClipboardEvent(`paste`):null`, `typeof
 * DOMException<"u"&&s instanceof DOMException`, `typeof ImageBitmap<"u"&&(...)` — which is the discipline
 * this file's own DEMOTING verdicts were held to and is required because a demotion's error is the one
 * nobody finds by acting on it.
 *
 * ARMED IN BOTH DIRECTIONS ON EVERY CONSTRUCTION, AND THE NEGATIVES OUTNUMBER THE POSITIVES BECAUSE THE
 * DEMOTING VERDICTS ARE THE DANGEROUS ONES. A classifier whose guard recognition silently stopped working
 * would report every site `throws`, which is loud and promotes; one whose polarity inverted would report a
 * flow-ender guarded, which is silent and removes a row from the queue. So every shape this file refuses to
 * call a guard is exercised and must answer `throws`, and a construction whose controls disagree THROWS
 * rather than returning a reader that cannot speak. */
import { parse } from "@babel/parser";

export const VERDICTS = ["throws", "caught", "guarded-fallback", "guarded-silent"];

/* The three receivers absentrank already treats as the global, so anchoring on them costs no new
   assumption. Spelled here rather than imported so this module has no dependency on its caller; the caller
   asserts the two agree. */
const GLOBAL_RECEIVERS = new Set(["window", "self", "globalThis"]);

/* Does this node DENOTE the global binding `name` in a position that cannot itself throw when it is absent?
   A bare identifier qualifies ONLY under `typeof`, which the caller below enforces by asking this of a
   `typeof` operand and of a member expression and never of a bare test. */
const isGlobalMember = (n, name) =>
  !!n && n.type === "MemberExpression" && n.object?.type === "Identifier"
  && GLOBAL_RECEIVERS.has(n.object.name)
  && ((!n.computed && n.property?.type === "Identifier" && n.property.name === name)
      || (n.computed && n.property?.type === "StringLiteral" && n.property.value === name));
/* THE VALUE OF A STRING LITERAL IN EITHER SPELLING. A minifier is free to emit a substitution-free template
   literal wherever the source wrote a quoted string, and several bundles in this corpus emit EVERY string
   that way — so a reader that asks only for `StringLiteral` is blind to a whole emitter's output rather than
   to an edge case, and the blindness is invisible because the shapes it can still see answer normally.
   absentrank records the same class omitting the BACKTICK from its own `"X" in global` channel, and records
   that its positive control missed it because the control was spelled with a double quote in the same
   breath as the pattern. The controls below are therefore spelled in BOTH delimiters. */
const stringValue = (n) =>
  n?.type === "StringLiteral" ? n.value
  : (n?.type === "TemplateLiteral" && n.expressions.length === 0 && n.quasis.length === 1
     && typeof n.quasis[0]?.value?.cooked === "string") ? n.quasis[0].value.cooked
  : null;
const typeofOperandIsName = (n, name) =>
  (n?.type === "Identifier" && n.name === name) || isGlobalMember(n, name);

/* presence(test, name): +1 when `test` being TRUE entails the name is defined, -1 when it entails the name
   is NOT defined, 0 when it entails neither. 0 is the answer for everything not enumerated, and 0 means the
   caller's occurrence is `throws`. */
function presence(t, name) {
  if (!t || typeof t !== "object" || typeof t.type !== "string") return 0;
  switch (t.type) {
    case "UnaryExpression":
      return t.operator === "!" ? -presence(t.argument, name) : 0;
    /* A member read on a global receiver cannot throw, so its truthiness is evidence the name is there. */
    case "MemberExpression":
      return isGlobalMember(t, name) ? 1 : 0;
    case "BinaryExpression": {
      /* ECMAScript §13.10.1 "Runtime Semantics: Evaluation" ends `in` at §7.3.11 "HasProperty ( obj,
         propertyKey )", so an absent name answers false and the operator never throws — which is why
         engine/absentrank.mjs already classes this spelling as a GUARD channel rather than a use. The titles
         are quoted because an unquoted one is prose to the citation audit and is judged by nothing. */
      if (t.operator === "in")
        return (t.left?.type === "StringLiteral" && t.left.value === name
                && t.right?.type === "Identifier" && GLOBAL_RECEIVERS.has(t.right.name)) ? 1 : 0;
      /* `typeof X < "u"` IS A PRESENCE TEST AND IS THE SPELLING A MINIFIER EMITS, NOT A CURIOSITY.
         ECMAScript §13.10.1 "Runtime Semantics: Evaluation" sends `<` to the abstract operation
         ECMAScript §7.2.12 "IsLessThan ( x, y, leftFirst )", which compares two Strings code unit by code
         unit and answers true for the shorter when one is a prefix of the other — so "u" is less than
         "undefined". ECMAScript §13.5.3 "The typeof Operator" fixes the operand's value set at exactly
         EIGHT strings, and of those eight only "undefined" fails to sort before "u", every other one
         beginning with b, f, n, o or s. So `typeof X < "u"` is `typeof X !== "undefined"` exactly, and
         `typeof X > "u"` is `typeof X === "undefined"` exactly. The eight were ENUMERATED and compared
         rather than reasoned about, which is the only way the next reader can check it; the controls below
         carry both polarities.
         THE OPERAND ORDER DECIDES THE POLARITY HERE WHERE IT DOES NOT FOR EQUALITY, so the side the
         `typeof` was found on is carried out of the loop rather than discarded: `"u" > typeof X` means
         DEFINED and `"u" < typeof X` means ABSENT, which is the pair an order-blind reader gets backwards
         in the silencing direction.
         `<=` AND `>=` ARE REFUSED although the same argument covers them, and the refusal is the measured
         arm rather than the timid one: over the corpus this landed against, `typeof X <= "u"` and
         `typeof X >= "u"` occur ZERO times in any spelling, so admitting them would be a rule with no site
         to be right about, and a refusal answers `throws`, which this file's bias says is the direction to
         err in. Every OTHER string is refused for a REAL reason and not for want of a measurement:
         "undefined" is less than "v", so `typeof X < "v"` holds whether or not X is defined and entails
         nothing at all. */
      if (t.operator === "<" || t.operator === ">") {
        for (const [a, b, typeofOnLeft] of [[t.left, t.right, true], [t.right, t.left, false]]) {
          if (a?.type !== "UnaryExpression" || a.operator !== "typeof") continue;
          if (!typeofOperandIsName(a.argument, name)) continue;
          if (stringValue(b) !== "u") return 0;
          return (typeofOnLeft ? t.operator === "<" : t.operator === ">") ? 1 : -1;
        }
        return 0;
      }
      if (!["===", "==", "!==", "!="].includes(t.operator)) return 0;
      const eq = t.operator === "===" || t.operator === "==";
      for (const [a, b] of [[t.left, t.right], [t.right, t.left]]) {
        if (a?.type !== "UnaryExpression" || a.operator !== "typeof") continue;
        if (!typeofOperandIsName(a.argument, name)) continue;
        const bv = stringValue(b);
        if (bv === null) return 0;
        if (bv === "undefined") return eq ? -1 : 1;
        /* `typeof X === "function"` entails defined; `typeof X !== "function"` entails nothing, because an
           absent name and a defined non-function both satisfy it. The asymmetry is the point. */
        return eq ? 1 : 0;
      }
      return 0;
    }
    case "LogicalExpression": {
      /* `a || b` being true entails only that ONE of them is, so it decides nothing. `a && b` being true
         entails both, so either side deciding is enough — and two sides deciding OPPOSITE ways is a
         contradiction this file refuses rather than resolves. */
      if (t.operator !== "&&") return 0;
      const l = presence(t.left, name), r = presence(t.right, name);
      if (l === 1 || r === 1) return (l === -1 || r === -1) ? 0 : 1;
      if (l === -1 || r === -1) return -1;
      return 0;
    }
    case "SequenceExpression":
      return presence(t.expressions[t.expressions.length - 1], name);
    default:
      return 0;
  }
}

/* AN IDENTIFIER NODE IS NOT ALWAYS A READ, WHICH THE ARMING FOUND RATHER THAN THE DESIGN ANTICIPATING IT.
   Babel spells a property KEY, a binding NAME and a LABEL as `Identifier` nodes, so an offset landing on one
   is an occurrence the program does not EVALUATE as this name and must reach no verdict — counting it
   `throws` would accuse a page of a ReferenceError it cannot raise. engine/js_code_refs.mjs draws the same
   line in its own header, from the other direction and by another mechanism: it marks `{X: 1}`, `class C {
   X(){} }` and `window.X = 1` and then refuses to call a mark `a read`, because "a property definition and
   a member write are not reads".
   THE POSITION THIS REACHES THAT THE CALLER CANNOT IS A FUNCTION PARAMETER, and that is not an invented
   case: absentrank's `f(a,X)` channel is anchored on `[\w$)\]]\s*\(`, which a `function f(a,X)` HEAD
   satisfies as readily as a call does — its own header records that shape as DISCHARGED BY CONSTRUCTION and
   says in the same breath that the discharge was never exercised on a site in that corpus. A parameter is a
   binding and never a free reference, so this test answers it positionally, and a caller's unlocated count
   is the first instrument that can say whether the population was ever empty.
   THE LIST IS ENUMERATED AND THE FLOOR IS STATED RATHER THAN IMPLIED: a binding position inside a
   DESTRUCTURING PATTERN is not covered and reads as a reference, which is the PROMOTING direction this file
   already takes everywhere else — an over-stated `throws` sends a reader to open a site, and the opposite
   removes one from the queue. */
const NOT_A_READ = (node, parent, key) => {
  if (!parent) return false;
  /* Asked FIRST and of every parent, because a method's key and a method's parameters are both reached from
     one node and a switch that answers about the key answers nothing about the params. */
  if (key === "params") return true;
  switch (parent.type) {
    case "MemberExpression": case "OptionalMemberExpression":
      return key === "property" && !parent.computed;
    case "ObjectProperty": case "ObjectMethod": case "ClassMethod": case "ClassPrivateMethod":
    case "ClassProperty": case "ClassPrivateProperty": case "ClassAccessorProperty":
      /* Shorthand `{X}` puts one identifier in BOTH slots and really does read X, so a key is a key only
         where it is not also the value. */
      return key === "key" && !parent.computed && parent.value !== node;
    case "VariableDeclarator":
      return key === "id";
    case "FunctionDeclaration": case "FunctionExpression": case "ClassDeclaration": case "ClassExpression":
      return key === "id";
    case "ImportSpecifier": case "ImportDefaultSpecifier": case "ImportNamespaceSpecifier":
    case "ExportSpecifier":
      return true;
    case "LabeledStatement":
      return key === "label";
    case "BreakStatement": case "ContinueStatement":
      return key === "label";
    default:
      return false;
  }
};

/* The innermost enclosing construct that decides. `stack` is the ancestor chain, outermost first, each entry
   carrying the node and the KEY of its parent that reached it — the key is what says which ARM an occurrence
   is in, and an arm is the whole of the question. */
function verdictOf(stack, name) {
  for (let i = stack.length - 1; i > 0; i--) {
    const key = stack[i].key, p = stack[i - 1].node;
    /* A `finally` with no `catch` re-raises, so the handler is required and not decoration. An occurrence in
       the HANDLER or the FINALIZER is not protected by this try at all, which the key test enforces. */
    if (p.type === "TryStatement" && key === "block" && p.handler) return "caught";
    if (p.type === "IfStatement" || p.type === "ConditionalExpression") {
      const g = presence(p.test, name);
      if (key === "consequent" && g === 1)
        return (p.type === "ConditionalExpression" || p.alternate) ? "guarded-fallback" : "guarded-silent";
      /* The arm taken when the test is FALSE is the guarded one exactly when the test being true means the
         name is absent; its other arm is the consequent, which a parsed `if` always has. */
      if (key === "alternate" && g === -1) return "guarded-fallback";
    }
    if (p.type === "LogicalExpression" && key === "right") {
      /* `a && use` reaches `use` when a is TRUE; `a || use` reaches it when a is FALSE. Nothing else runs in
         either case, so neither carries a fallback arm. */
      if (p.operator === "&&" && presence(p.left, name) === 1) return "guarded-silent";
      if (p.operator === "||" && presence(p.left, name) === -1) return "guarded-silent";
    }
  }
  return "throws";
}

/* Keys that carry no AST child and whose traversal would be wasted or wrong. `extra` holds a literal's raw
   text and parenthesisation, `loc`/`range` hold positions; comments are attached only when asked for. */
const SKIP = new Set(["type", "start", "end", "loc", "range", "extra",
                      "leadingComments", "trailingComments", "innerComments"]);

function classifyIn(src, targets) {
  let ast;
  try {
    ast = parse(src, { sourceType: "unambiguous", errorRecovery: false, attachComment: false });
  } catch (e) {
    return { parsed: false, why: `parse: ${String(e && e.message).slice(0, 80)}`, verdicts: new Map() };
  }
  const verdicts = new Map();
  const stack = [];
  const walk = (node, key) => {
    if (!node || typeof node !== "object") return;
    if (Array.isArray(node)) { for (const c of node) walk(c, key); return; }
    if (typeof node.type !== "string" || node.start === undefined) return;
    const parent = stack.length ? stack[stack.length - 1].node : null;
    stack.push({ node, key });
    if (node.type === "Identifier" && targets.get(node.start) === node.name
        && !NOT_A_READ(node, parent, key))
      verdicts.set(node.start, verdictOf(stack, node.name));
    for (const k of Object.keys(node)) {
      if (SKIP.has(k)) continue;
      const v = node[k];
      if (v && typeof v === "object") walk(v, k);
    }
    stack.pop();
  };
  try {
    walk(ast, null);
  } catch (e) {
    /* A minified bundle can nest deeper than this recursion, and a walk that died has seen an unknown part
       of the file — so its partial verdicts are discarded rather than reported beside complete ones. */
    return { parsed: false, why: `walk: ${String(e && e.message).slice(0, 80)}`, verdicts: new Map() };
  }
  return { parsed: true, why: null, verdicts };
}

/* EVERY SHAPE THIS FILE REFUSES TO CALL A GUARD IS EXERCISED, AND IT IS THE LONGER HALF OF THE TABLE. The
   positives prove the classifier can speak; the negatives prove it refuses the things whose demotion would
   remove a real flow-ender from the queue, which is the failure nobody discovers by acting on it. */
const ARM = [
  /* --- it can speak ---------------------------------------------------------------------------------- */
  ["new X(1)",                                        "throws"],
  ["try{new X(1)}catch(e){}",                         "caught"],
  ["if(typeof X!=='undefined'){new X(1)}",            "guarded-silent"],
  ["if(typeof X!=='undefined'){new X(1)}else{fb()}",  "guarded-fallback"],
  ["if(typeof X=='undefined'){fb()}else{new X(1)}",   "guarded-fallback"],
  ["typeof X!=='undefined'?new X(1):fb()",            "guarded-fallback"],
  ["typeof X!=='undefined'&&new X(1)",                "guarded-silent"],
  ["typeof X==='undefined'||new X(1)",                "guarded-silent"],
  ["if('X' in window){new X(1)}",                     "guarded-silent"],
  ["if(window.X){new X(1)}",                          "guarded-silent"],
  ["if(globalThis['X']){new X(1)}",                   "guarded-silent"],
  ["if(typeof X==='function'){new X(1)}",             "guarded-silent"],
  ["if(typeof self.X!=='undefined'){new X(1)}",       "guarded-silent"],
  ["if(!(typeof X==='undefined')){new X(1)}",         "guarded-silent"],
  ["if(cond&&typeof X!=='undefined'){new X(1)}",      "guarded-silent"],
  /* The relational spelling, in both polarities, both operand orders and both string delimiters. */
  ["if(typeof X<'u'){new X(1)}",                      "guarded-silent"],
  ["typeof X<'u'&&new X(1)",                          "guarded-silent"],
  ["typeof X<`u`&&new X(1)",                          "guarded-silent"],
  ["typeof X>'u'||new X(1)",                          "guarded-silent"],
  ["typeof X<'u'?new X(1):fb()",                      "guarded-fallback"],
  ["if(typeof X>'u'){fb()}else{new X(1)}",            "guarded-fallback"],
  ["'u'>typeof X&&new X(1)",                          "guarded-silent"],
  ["if('u'<typeof X){fb()}else{new X(1)}",            "guarded-fallback"],
  ["if(typeof self.X<'u'){new X(1)}",                 "guarded-silent"],
  /* The equality arm, in the delimiter it could not read before. */
  ["if(typeof X!==`undefined`){new X(1)}",            "guarded-silent"],
  ["if(typeof X===`function`){new X(1)}",             "guarded-silent"],
  /* --- and it refuses -------------------------------------------------------------------------------- */
  /* Polarity: the use sits in the arm taken when the name is ABSENT. Demoting either would delete a real
     flow-ender, which is the one error this classifier must not make. */
  ["if(typeof X==='undefined'){new X(1)}",            "throws"],
  ["typeof X!=='undefined'||new X(1)",                "throws"],
  /* THE TEST ITSELF EVALUATES THE BINDING, so the ReferenceError lands before any arm is entered. */
  ["if(X){new X(1)}",                                 "throws"],
  ["X&&new X(1)",                                     "throws"],
  /* A test of a DIFFERENT name, and a member read off something that is not a global receiver. */
  ["if(typeof Y!=='undefined'){new X(1)}",            "throws"],
  ["if(opts.X){new X(1)}",                            "throws"],
  ["if('X' in opts){new X(1)}",                       "throws"],
  /* `!== "function"` is satisfied by an absent name as well as by a defined non-function. */
  ["if(typeof X!=='function'){new X(1)}",             "throws"],
  /* The relational spelling with the polarity inverted — the use sits in the arm taken when X is ABSENT. */
  ["if(typeof X>'u'){new X(1)}",                      "throws"],
  ["typeof X<'u'||new X(1)",                          "throws"],
  ["if('u'<typeof X){new X(1)}",                      "throws"],
  /* A relational test against ANY other string decides nothing: "undefined" < "v" is true, so this holds
     whether or not X is defined. A reader that keyed on the operator rather than on the operand would
     demote a real flow-ender here. */
  ["if(typeof X<'v'){new X(1)}",                      "throws"],
  /* `<=` and `>=` are not read — measured at zero occurrences, so the rule would have no site to be right
     about, and the refusal is the promoting direction. */
  ["if(typeof X<='u'){new X(1)}",                     "throws"],
  ["if(typeof X>='u'){new X(1)}",                     "throws"],
  /* A template literal that is not a plain string is not one — its value is not known at parse time. */
  ["if(typeof X<`${a}`){new X(1)}",                   "throws"],
  ["if(typeof X!==`undefined${a}`){new X(1)}",        "throws"],
  /* A try whose throw is NOT caught here: no handler, or the occurrence in the handler / the finalizer. */
  ["try{new X(1)}finally{}",                          "throws"],
  ["try{}catch(e){new X(1)}",                         "throws"],
  ["try{}finally{new X(1)}",                          "throws"],
  /* Lexical containment and not control-flow dominance — stated in the header and exercised here. */
  ["if(typeof X!=='undefined'){}new X(1)",            "throws"],
  /* A guard the parser sees as a string rather than as a test. */
  ["if('typeof X!==\"undefined\"'){new X(1)}",        "throws"],
];
/* A target offset that lands on something that is not an Identifier of that name gets NO verdict, and the
   caller counts it as unlocated. Exercised so that a reader whose location silently stopped working — which
   would report every site unlocated and empty the table — is a THROW rather than a clean bill. */
const ARM_UNLOCATED = [
  "var s='new X(1)'",          /* inside a string        */
  "/* new X(1) */ 0",          /* inside a comment       */
  "var o={X:1}",               /* an object literal key  */
  "o.X",                       /* a member property      */
  "o?.X",                      /* an optional-chained one */
  "class C{X(){}}",            /* a method name          */
  "function f(a,X){}",         /* a PARAMETER — the position absentrank's `f(a,X)` channel can reach */
  "var X=1",                   /* a binding name         */
  "function X(){}",            /* a function declaration */
  "class X{}",                 /* a class declaration    */
  "import {X} from 'm'",       /* an import specifier    */
  "({m(a,X){}})",              /* an OBJECT METHOD's parameter — the case a key-first switch answers wrongly */
  "class C{m(a,X){}}",         /* a class method's parameter, same shape */
  "((a,X)=>0)",                /* an arrow's parameter */
];
/* The reference positions the caller's four USE channels actually aim at, exercised so that the test above
   cannot start refusing them — which would empty the table while every control that speaks still speaks. */
const ARM_LOCATED = ["new X(1)", "a instanceof X", "X.m", "f(a,X)", "typeof X", "[X]", "({m:X})"];

export function guardShapeReader() {
  const die = (s) => { throw new Error(`[js_guard_shape] ${s}`); };
  const at = (src) => {
    const i = src.indexOf("new X(");
    return i < 0 ? die(`control ${JSON.stringify(src)} does not contain the probe`) : i + 4;
  };
  for (const [src, want] of ARM) {
    const off = at(src);
    const { parsed, why, verdicts } = classifyIn(src, new Map([[off, "X"]]));
    if (!parsed) die(`control ${JSON.stringify(src)} did not parse (${why}) — its verdict would mean nothing.`);
    const got = verdicts.get(off);
    if (got !== want)
      die(`control ${JSON.stringify(src)} wanted ${want} and answered ${got || "no verdict"}. ` +
          (want === "throws"
            ? "This classifier calls a use GUARDED that is not, which removes a flow-ender from the band a "
              + "reader dispatches from and is the one error nobody finds by acting on it."
            : "This classifier can no longer see the shape it exists to see, so every site would read "
              + "`throws` and the split would be a column of one number."));
  }
  for (const src of ARM_UNLOCATED) {
    /* These carry no use position — the point is that the only `X` in them is not one. So the offset is the
       first `X` in the text, which is what a channel regex would have handed over. */
    const off = src.indexOf("X");
    if (off < 0) die(`unlocated control ${JSON.stringify(src)} does not contain the probe`);
    const { parsed, verdicts } = classifyIn(src, new Map([[off, "X"]]));
    if (!parsed) die(`unlocated control ${JSON.stringify(src)} did not parse.`);
    if (verdicts.has(off))
      die(`unlocated control ${JSON.stringify(src)} was given the verdict ${verdicts.get(off)} — an offset ` +
          `inside a string, a comment or a property key is not a use and must reach no verdict at all.`);
  }
  for (const src of ARM_LOCATED) {
    const off = src.lastIndexOf("X");
    const { parsed, verdicts } = classifyIn(src, new Map([[off, "X"]]));
    if (!parsed) die(`located control ${JSON.stringify(src)} did not parse.`);
    if (!verdicts.has(off))
      die(`located control ${JSON.stringify(src)} reached no verdict — this reader has stopped seeing a ` +
          `position the caller's channels aim at, and every site there would read as unlocated.`);
  }
  return { classify: classifyIn, armed: ARM.length + ARM_UNLOCATED.length + ARM_LOCATED.length,
           receivers: GLOBAL_RECEIVERS };
}
