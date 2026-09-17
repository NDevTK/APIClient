// render_diff.js — the RENDER DIFFERENTIAL: a per-element USED-GEOMETRY dump that real Chrome and this engine
// can each produce, a comparator that joins two of them and says where they disagree, and a renderer that
// DRAWS them side by side for a human.
//
// TWO ARTIFACTS, TWO JOBS, AND NEITHER MAY MASQUERADE AS THE OTHER. The comparison SCORES — its output is a
// number with a denominator and a list of element keys. The drawing READS — its output is something a person
// looks at. They are built from the SAME rows and the drawing takes its colours from the comparison's own
// verdicts, so the picture cannot say something the score does not; what they must never become is one
// artifact, because a divergence count that is really a picture cannot be checked and a picture that is really
// a score is believed on sight.
//
// WHY A GEOMETRY DUMP AND NOT A PICTURE. The goal is to spot differences, and a difference you cannot NAME is
// not one anybody can fix: a pixel count localises to a rectangle, and a rectangle localises to nothing. This
// artifact's divergences name an ELEMENT and a FIELD. The raster is not "the same thing later" either — it has
// no engine side at all, because engine/host/browser/core/paint/ holds a stacking order and no rasteriser, so
// a raster oracle would be one-sided at every ordering. When one exists, its OWN localiser is this artifact:
// mapping a differing pixel back to an element is exactly the join below.
//
// THE TWO SIDES RUN THE SAME PROGRAM. `COLLECTOR` is one function, shipped as source, evaluated in the page.
// Chrome runs it through this file; the engine will run it as page script. There is no second implementation
// of the artifact's shape to drift, which is the whole of design constraint (1): a comparator whose two inputs
// were built by two programs is measuring those programs.
//
// THE VIEWPORT IS PART OF THE ARTIFACT AND A MISMATCH IS A REFUSAL, NOT A ROW. CSS 2.1 §9.1.1 "The viewport"
// says user agents "may change the document's layout when the viewport is resized", so two dumps taken at two
// viewports are two layouts and every row disagrees for a reason that is not a fidelity bug. This engine's
// top-level traversable is 1280 x 720 (VIEWPORT_TOP_WIDTH / VIEWPORT_TOP_HEIGHT in
// engine/host/browser/core/frame/viewport.c), so that is what `collect` pins Chrome to. It pins the LAYOUT
// viewport directly rather than sizing the window: the engine also models a window chrome
// (VIEWPORT_CHROME_HEIGHT), and that is the engine's PICKED number rather than the one the real browser has,
// so deriving `--window-size` from it would miss by the difference between two user agents' chrome and call
// the result pinned. RETIREMENT: this paragraph goes when the collector's viewport is asserted equal to a
// constant the engine EMITS, so the two cannot be edited apart.
//
// A MISSING VALUE IS NOT A ZERO. CSSOM VIEW §6 "Extensions to the Element Interface" makes a zero rectangle a
// real ANSWER — "if the list is empty, return a DOMRect object whose x, y, width and height members are zero"
// — so an element that generates no box agrees at zero on both sides and that agreement is information. What
// must never be written as zero is a value the producer did not produce: those rows carry a positive
// `unanswered` reason and are counted in their own column. There is no `|| 0` and no `??` in this file.
//
// THE TRY/CATCH IN THE COLLECTOR IS NOT A SWALLOWED INVARIANT. CLAUDE.md forbids catching an engine invariant;
// this catch cannot reach one, because a DCHECK/DFAIL in a dev build ABORTS THE PROCESS and no JS handler runs.
// What it does catch is a member that really threw, and it records the throw as a row rather than discarding
// the element — which is the opposite of swallowing. RETIREMENT: this note goes when the collector records a
// throw through a shared reporter that cannot also be read as a guard.
//
// IT DOES NOT `require` harness.js. That file calls `main()` at its top level, so importing it IS running it —
// the defect CLAUDE.md records for `.mjs` files whose top level is the work. The lock/port convention below is
// harness.js's, re-read rather than re-implemented: the same `HARNESS_LOCK` / `HARNESS_PORT` names, so a
// private browser is one variable away here exactly as it is there.

"use strict";

const fs = require("fs");
const path = require("path");
const puppeteer = require("puppeteer");

/* THE ENGINE'S OWN VIEWPORT, COPIED WITH ITS SOURCE NAMED. This is a SECOND COPY of a number the engine owns,
   which is a thing this project distrusts on principle — so it is stated once, here, with the file it was read
   from, and `compare` refuses rather than averaging when two artifacts disagree about it. The refusal is what
   makes the copy safe: a drift cannot produce rows, it can only produce a refusal naming both numbers. */
const ENGINE_VIEWPORT = { width: 1280, height: 720 };   /* core/frame/viewport.c VIEWPORT_TOP_{WIDTH,HEIGHT} */

const LOCK_FILE = process.env.HARNESS_LOCK
  ? path.resolve(process.env.HARNESS_LOCK) : path.join(__dirname, "harness.lock");
const DEFAULT_PORT = Number(process.env.HARNESS_PORT || 9337);

function fail(msg) { throw new Error("@WHY render_diff: " + msg); }
function check(cond, msg) { if (!cond) fail(msg); }

/* ── the ONE collector ─────────────────────────────────────────────────────────────────────────────────────
 *
 * A pure page script. It reaches for exactly four platform facts and nothing else, so the engine side needs no
 * selector engine and no serialiser to run it: the element tree (`documentElement` and `children`);
 * CSSOM VIEW §6 "Extensions to the Element Interface"'s `getBoundingClientRect`;
 * CSSOM VIEW §4 "Extensions to the Window Interface"'s `innerWidth` and `innerHeight`, which "must return the
 * viewport width, including the size of a rendered scroll bar (if any), or zero if there is no viewport";
 * and `location.href`. Each standard is NAMED at its own number rather than carried from the line above —
 * a resolver takes the NEAREST named standard, so a quotation after a list of them is judged against whichever
 * came last.
 *
 * IT RETURNS AN OBJECT RATHER THAN A STRING, DELIBERATELY. Every number in a rectangle this engine reports is
 * a CONCOLIC whose example is derived from the initial containing block, so composing one into a string is the
 * defect CLAUDE.md §A-WITNESS-MAY-NOT-BE-COMPOSED records: the payload never becomes concrete and the witness
 * silently never fires. Keeping the collector's output an object leaves the serialisation to whoever is
 * driving it, which is where that problem actually lives and where it has to be solved once.
 *
 * THE KEY IS STRUCTURAL, NOT AN ORDINAL INTO A LIST. A row is named by its path from the root — each step a
 * tag name and its index among its parent's ELEMENT children. Two walks that disagree about the tree then
 * disagree about KEYS, so a parser divergence surfaces as rows present on one side only, in its own column,
 * instead of silently shifting every geometry row after it and reporting N mismatches for one defect. That is
 * the same reason CLAUDE.md gives for keying a replayed decision on a name rather than on a position: an index
 * names a thing only while the set is fixed. */
function COLLECTOR() {
  var rows = [];
  var seen = Object.create(null);

  function record(el, key) {
    var f = null, un = null;
    try {
      var r = el.getBoundingClientRect();
      /* The four members CSSOM VIEW §6 "Extensions to the Element Interface" names, each read separately: a
         rectangle that answered some members and not others is a row with some fields and an `unanswered`
         reason, never a row with zeros in the gaps. */
      f = { x: r.x, y: r.y, width: r.width, height: r.height };
      for (var k in f) {
        if (typeof f[k] !== "number") { un = "getBoundingClientRect()." + k + " is not a Number"; f = null; break; }
      }
    } catch (e) {
      un = "getBoundingClientRect() threw: " + (e && e.name ? e.name : "?") + ": " + (e && e.message ? e.message : "");
    }
    rows.push({ key: key, tag: el.tagName ? el.tagName.toLowerCase() : "?", fields: f, unanswered: un });
  }

  function walk(el, key) {
    if (seen[key]) throw new Error("render_diff collector: two elements share the path key " + key +
                                   " — a path is unique by construction, so the walk is wrong");
    seen[key] = true;
    record(el, key);
    var kids = el.children, i, n = kids ? kids.length : 0;
    for (i = 0; i < n; i++) {
      var c = kids[i];
      walk(c, key + "/" + (c.tagName ? c.tagName.toLowerCase() : "?") + "[" + i + "]");
    }
  }

  var root = document.documentElement;
  if (root) walk(root, (root.tagName ? root.tagName.toLowerCase() : "?") + "[0]");

  return {
    artifact: "render-geometry",
    version: 1,
    url: String(location.href),
    viewport: { width: innerWidth, height: innerHeight },
    devicePixelRatio: devicePixelRatio,
    elementCount: rows.length,
    rows: rows
  };
}

/* The source a driver evaluates. One function, one `.toString()`, so what Chrome runs and what the engine will
   run are the same bytes and a reader can print them (`render_diff.js collector`). */
function collectorSource() {
  return "(" + COLLECTOR.toString() + ")()";
}

/* ── the artifact ──────────────────────────────────────────────────────────────────────────────────────────
 *
 * Asserted on the way in rather than trusted: a consumer that defaults a producer's field cannot tell a
 * producer that stopped writing it from one that wrote a zero. */
/* THE COLLECTOR OWES THE BODY; THE DRIVER OWES THE STAMP — two obligations, asserted apart, because the page
   genuinely cannot know which engine is running it. `producer` is a fact about the DRIVE and an artifact body
   that claimed one would be guessing. Splitting them lets an engine-side driver validate what came out of the
   page BEFORE it stamps, so a collector that half-ran is caught at the page boundary rather than at the
   comparison, where it would read as a tree divergence.
   RETIREMENT: this split goes when the two are one call because nothing produces an unstamped body — which
   will not happen while the collector runs in a realm that is not the driver's. */
function checkArtifactBody(a, where) {
  check(a && typeof a === "object", where + " is not an object");
  check(a.artifact === "render-geometry", where + " is not a render-geometry artifact (artifact=" + a.artifact + ")");
  check(a.version === 1, where + " is version " + a.version + " and this comparator reads version 1");
  check(a.viewport && typeof a.viewport.width === "number" && typeof a.viewport.height === "number",
        where + " states no viewport — and a geometry artifact whose viewport is unknown cannot be compared " +
        "with anything, because CSS 2.1 §9.1.1 \"The viewport\" makes the layout a function of it");
  check(Array.isArray(a.rows), where + " has no rows array");
  check(a.elementCount === a.rows.length,
        where + " says elementCount " + a.elementCount + " and carries " + a.rows.length + " rows — a count " +
        "that disagrees with the list it counts was wrong when it was written");
  return a;
}

/* A FINISHED artifact: a body plus the one fact only the driver holds. */
function checkArtifact(a, where) {
  checkArtifactBody(a, where);
  check(typeof a.producer === "string" && a.producer.length > 0,
        where + " names no producer — a row's worth depends on which engine made it, and an unstamped " +
        "artifact compared against a stamped one reports a divergence with no side to attribute it to");
  return a;
}

/* ── the comparator ────────────────────────────────────────────────────────────────────────────────────────
 *
 * Joins by key and sorts every key into exactly ONE of five disjoint columns, which is design constraint (3):
 * a value nobody produced and a value that disagrees are different facts and are never summed. The columns are
 * asserted to sum to the size of the key union, so a row that fell through every arm cannot go unnoticed.
 *
 * FIELDS ARE COMPARED BY NAME, so a second channel — a computed-style value, a paint index — is a new key in
 * `fields` and needs no change here. That is a property of this function and not a promise: it never mentions
 * `x`, `y`, `width` or `height`.
 *
 * THE TOLERANCE IS A CLAIM AND IT IS PRINTED. It defaults to 0 because two dumps of one engine at one viewport
 * are bit-identical, and a default tolerance is precisely the plausible datum that hides a real divergence; any
 * other value is an explicit argument and travels with every number this function reports. */
function compare(A, B, tolerance) {
  checkArtifact(A, "artifact A"); checkArtifact(B, "artifact B");
  check(typeof tolerance === "number" && isFinite(tolerance) && tolerance >= 0,
        "tolerance must be a finite non-negative number of CSS pixels, got " + tolerance);
  if (A.viewport.width !== B.viewport.width || A.viewport.height !== B.viewport.height) {
    fail("REFUSED: artifact A was taken at " + A.viewport.width + "x" + A.viewport.height + " and artifact B " +
         "at " + B.viewport.width + "x" + B.viewport.height + ". Those are two layouts, so every row would " +
         "disagree for a reason that is not a fidelity bug. Re-collect both at " + ENGINE_VIEWPORT.width + "x" +
         ENGINE_VIEWPORT.height + ", which is what this engine's top-level traversable models");
  }

  const ia = new Map(), ib = new Map();
  for (const r of A.rows) ia.set(r.key, r);
  for (const r of B.rows) ib.set(r.key, r);
  check(ia.size === A.rows.length, "artifact A holds a duplicate path key — a path is unique by construction");
  check(ib.size === B.rows.length, "artifact B holds a duplicate path key — a path is unique by construction");

  const out = { tolerance, viewport: A.viewport, agree: [], differ: [], unanswered: [], onlyA: [], onlyB: [] };
  const keys = new Set([...ia.keys(), ...ib.keys()]);

  for (const k of keys) {
    const a = ia.get(k), b = ib.get(k);
    if (a === undefined) { out.onlyB.push({ key: k, tag: b.tag }); continue; }
    if (b === undefined) { out.onlyA.push({ key: k, tag: a.tag }); continue; }
    if (a.unanswered !== null || b.unanswered !== null) {
      out.unanswered.push({ key: k, tag: a.tag, a: a.unanswered, b: b.unanswered });
      continue;
    }
    check(a.fields && b.fields,
          "a row with no `unanswered` reason and no fields is neither an answer nor a stated absence, at " + k);
    const names = new Set([...Object.keys(a.fields), ...Object.keys(b.fields)]);
    const bad = [];
    let stated = false;   /* one side stated it had no value for a field: an ABSENCE, never a mismatch at zero */
    for (const n of names) {
      const va = a.fields[n], vb = b.fields[n];
      /* A FIELD ONE SIDE HAS AND THE OTHER DOES NOT IS A STATED ABSENCE, not a mismatch against an implied
         zero — the same rule as a missing row, one level down. */
      if (typeof va !== "number" || typeof vb !== "number") {
        out.unanswered.push({ key: k, tag: a.tag,
                              a: typeof va === "number" ? null : "field " + n + " absent in A",
                              b: typeof vb === "number" ? null : "field " + n + " absent in B" });
        stated = true;
        break;
      }
      if (Math.abs(va - vb) > tolerance) bad.push({ field: n, a: va, b: vb, delta: vb - va });
    }
    if (stated) continue;
    if (bad.length === 0) out.agree.push({ key: k, tag: a.tag });
    else out.differ.push({ key: k, tag: a.tag, fields: bad });
  }

  const sum = out.agree.length + out.differ.length + out.unanswered.length + out.onlyA.length + out.onlyB.length;
  check(sum === keys.size,
        "the columns sum to " + sum + " and the key union holds " + keys.size + " — a key fell through every " +
        "arm, so at least one row is in no column and the coverage figure below is a fraction of nothing");
  out.keys = keys.size;
  return out;
}

/* A COVERAGE FIGURE STATES WHAT IT IS A FRACTION OF, on the same line — design constraint (5). It also states
   the tolerance, because two runs at two tolerances are two questions and the digits alone cannot say which. */
function report(r, labelA, labelB) {
  const lines = [];
  lines.push("render-geometry differential  " + labelA + "  vs  " + labelB);
  lines.push("  viewport " + r.viewport.width + "x" + r.viewport.height + "   tolerance " + r.tolerance + "px");
  lines.push("  compared " + (r.agree.length + r.differ.length) + "/" + r.keys + " elements present in either side");
  lines.push("    agree       " + r.agree.length);
  lines.push("    differ      " + r.differ.length);
  lines.push("    unanswered  " + r.unanswered.length + "   (a producer stated it had no value — never a zero)");
  lines.push("    only in A   " + r.onlyA.length + "   (tree divergence: the key exists in A alone)");
  lines.push("    only in B   " + r.onlyB.length + "   (tree divergence: the key exists in B alone)");
  for (const d of r.differ.slice(0, 40)) {
    for (const f of d.fields) {
      lines.push("  DIFFER " + d.key + "  " + f.field + "  A=" + f.a + "  B=" + f.b + "  delta=" + f.delta);
    }
  }
  if (r.differ.length > 40) lines.push("  … " + (r.differ.length - 40) + " further differing elements not printed");
  for (const u of r.unanswered.slice(0, 20)) lines.push("  UNANSWERED " + u.key + "  A=" + u.a + "  B=" + u.b);
  if (r.unanswered.length > 20) lines.push("  … " + (r.unanswered.length - 20) + " further unanswered not printed");
  for (const o of r.onlyA.slice(0, 20)) lines.push("  ONLY-IN-A " + o.key);
  for (const o of r.onlyB.slice(0, 20)) lines.push("  ONLY-IN-B " + o.key);
  return lines.join("\n");
}

/* ── the VIEWABLE artifact ─────────────────────────────────────────────────────────────────────────────────
 *
 * A picture is a SECOND JOB, not the same job drawn: the comparison above SCORES and this READS. Saying which
 * is which is the whole discipline, because an image is the most persuasive thing in the room and a rendered
 * lie is more convincing than a wrong number — a wrong layout draws a wrong picture that looks entirely
 * plausible, which is the plausible-datum defect arriving as something a human trusts on sight.
 *
 * SO THE PICTURE IS A FUNCTION OF THE ARTIFACT, NEVER A SECOND EMITTER IN THE ENGINE. Drawing from the engine's
 * own geometry would be a SECOND producer of the same fact, free to drift from the one the comparator scores —
 * the dual-system rot, with the two copies disagreeing where nobody is looking. Rendering from the artifact
 * buys three things instead: ONE renderer draws both sides, so the two pictures are comparable by construction
 * exactly as the one collector makes the two artifacts comparable; the engine's whole obligation stays
 * "produce the artifact" and the image costs it NO further C; and the picture and the score are derived from
 * the same rows, so they cannot disagree.
 *
 * AND A SCREENSHOT IS NOT THE OTHER HALF OF THIS COMPARISON. A Chrome PNG and a box diagram are two KINDS of
 * artifact, and comparing them is the mistake this whole file is built to avoid — it would also mislead in the
 * flattering direction, since the diagram looks clean and the photograph looks real. `shot` exists because a
 * real capture is worth having and is what a person means by "show me the page", and it is labelled a
 * REFERENCE: it is never an input to `compare` and there is no code path by which it becomes one.
 *
 * ABSENCE IS DRAWN AS ABSENCE, AND THE HAZARD IS THE REVERSE OF THE OBVIOUS ONE. CSSOM VIEW §6 "Extensions to
 * the Element Interface" makes a zero-area rectangle a real ANSWER, so an empty box here is CORRECT and must
 * stay visible rather than vanishing into the background — it gets a marker. What must never be drawn at all is
 * a row whose producer stated it had no value: it has no box, drawing one would invent geometry, and omitting
 * it silently would read as "nothing is here". Those rows go in a LEDGER under the picture, with their key and
 * the reason, so the count under the drawing and the count in the score are the same count.
 * RETIREMENT: this paragraph goes when a renderer cannot be written that drops a row — when every row reaches
 * the output as a mark or as a ledger line by construction rather than because this says so. */

const PANEL_PAD = 24, LEDGER_LINE = 13, LEDGER_HEAD = 34;

function esc(t) {
  return String(t).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
                  .replace(/"/g, "&quot;");
}

/* The verdict a row is DRAWN with comes from `compare`, so the picture cannot say something the score does not.
   A renderer that classified rows itself would be a second comparator — the same second copy the header above
   refuses for geometry, one layer up. */
const VERDICT_STYLE = {
  agree:      { stroke: "#5b6b7a", width: 0.6, fill: "none",              label: "agrees with the other side" },
  differ:     { stroke: "#d0021b", width: 1.6, fill: "rgba(208,2,27,.10)", label: "DIFFERS — see the ledger" },
  unanswered: { stroke: "#f5a623", width: 1.2, fill: "none",              label: "producer stated no value" },
  only:       { stroke: "#0b6cff", width: 1.2, fill: "rgba(11,108,255,.10)", label: "present on this side only" },
};

/* ONE PANEL: the rows of one artifact, at that artifact's own viewport, each row carrying a verdict. `verdicts`
   is a Map from key to a VERDICT_STYLE name; a key the map does not hold is drawn as `agree`, which is the
   right default ONLY because the sole caller builds the map from `compare`'s five columns, which are asserted
   to cover the key union. There is no other caller, and a second one would have to satisfy that or the drawing
   would quietly invent a verdict. */
function svgPanel(art, verdicts, originX, originY) {
  const out = [];
  const W = art.viewport.width, H = art.viewport.height;
  out.push('<g transform="translate(' + originX + ',' + originY + ')">');
  /* THE PANEL FRAME AND AN ELEMENT BOX ARE BOTH RECTANGLES, so each says which it is. A reader — or a check —
     counting element boxes out of this document would otherwise count the frame as one, which is a small
     number wrong in the direction that makes a drawing look like it holds one more element than it does. */
  out.push('<rect class="frame" x="0" y="0" width="' + W + '" height="' + H + '" fill="#ffffff" ' +
           'stroke="#222" stroke-width="1"/>');
  out.push('<text x="0" y="-8" font-family="monospace" font-size="13" fill="#222">' +
           esc(art.producer) + "  " + W + "x" + H + "  " + esc(art.url) + '</text>');
  let drawn = 0, zero = 0, held = 0;
  for (const r of art.rows) {
    if (r.fields === null) { held++; continue; }   /* a stated absence has no box; the ledger carries it */
    const st = VERDICT_STYLE[verdicts.get(r.key) || "agree"];
    const w = r.fields.width, h = r.fields.height;
    out.push('<rect class="el" x="' + r.fields.x + '" y="' + r.fields.y + '" width="' + Math.max(w, 0) +
             '" height="' + Math.max(h, 0) + '" fill="' + st.fill + '" stroke="' + st.stroke +
             '" stroke-width="' + st.width + '"><title>' + esc(r.key) + "  " + w + "x" + h +
             " @ " + r.fields.x + "," + r.fields.y + '</title></rect>');
    drawn++;
    /* A ZERO-AREA BOX IS AN ANSWER AND MUST NOT VANISH. The empty-list arm of CSSOM VIEW §6 "Extensions to
       the Element Interface" returns exactly this, so it is CORRECT and a reader has to be able to SEE that
       the engine said it — a rectangle of no area draws no pixels, which reads identically to an element
       that was never in the list. */
    if (w === 0 || h === 0) {
      zero++;
      out.push('<circle cx="' + r.fields.x + '" cy="' + r.fields.y + '" r="2.5" fill="none" stroke="' +
               st.stroke + '" stroke-width="1"><title>' + esc(r.key) + " — zero-area box (a real answer: " +
               'CSSOM VIEW §6 "Extensions to the Element Interface" returns one for an element ' +
               'with no fragments)</title></circle>');
    }
  }
  out.push("</g>");
  return { xml: out.join("\n"), drawn, zero, held };
}

function svgLedger(lines, originX, originY, width) {
  const out = ['<g transform="translate(' + originX + ',' + originY + ')">'];
  let y = 0;
  for (const l of lines) {
    out.push('<text x="0" y="' + y + '" font-family="monospace" font-size="11" fill="' + (l.fill || "#222") +
             '">' + esc(l.text) + "</text>");
    y += LEDGER_LINE;
  }
  out.push("</g>");
  return { xml: out.join("\n"), height: y, width };
}

/* ONE ARTIFACT, DRAWN. Every row reaches the output: as a rectangle, or as a ledger line saying why it has no
   rectangle. The two counts are printed, so a reader can add them up against `elementCount` without trusting
   the drawing. */
function svgOne(art) {
  checkArtifact(art, "the artifact being drawn");
  const W = art.viewport.width, H = art.viewport.height;
  const panel = svgPanel(art, new Map(), PANEL_PAD, PANEL_PAD + LEDGER_HEAD);
  const held = art.rows.filter((r) => r.fields === null);
  const lines = [
    { text: "drawn " + panel.drawn + " + stated-absent " + panel.held + " = " + art.elementCount +
            " elements   (" + panel.zero + " of the drawn are zero-area, which is an ANSWER and is ringed)" },
  ];
  for (const r of held.slice(0, 40)) {
    lines.push({ text: "ABSENT  " + r.key + "  " + r.unanswered, fill: "#f5a623" });
  }
  if (held.length > 40) lines.push({ text: "… " + (held.length - 40) + " further stated absences",
                                     fill: "#f5a623" });
  const led = svgLedger(lines, PANEL_PAD, PANEL_PAD + LEDGER_HEAD + H + 26, W);
  const totalH = PANEL_PAD * 2 + LEDGER_HEAD + H + 26 + led.height;
  return svgDoc(W + PANEL_PAD * 2, totalH, panel.xml + "\n" + led.xml);
}

/* TWO ARTIFACTS, SIDE BY SIDE, COLOURED BY THE SCORE. This is the artifact a person LOOKS at, and it is the
   same five columns the report prints — so "how many differ" and "which boxes are red" are one number asked
   twice. It runs `compare`, which means it also inherits the viewport REFUSAL: two layouts are never drawn as
   if they were one page seen twice. */
function svgCompare(A, B, tolerance) {
  const r = compare(A, B, tolerance);
  const verdicts = new Map();
  for (const d of r.differ) verdicts.set(d.key, "differ");
  for (const u of r.unanswered) verdicts.set(u.key, "unanswered");
  for (const o of r.onlyA) verdicts.set(o.key, "only");
  for (const o of r.onlyB) verdicts.set(o.key, "only");
  const W = A.viewport.width, H = A.viewport.height;
  const gap = 40;
  const left = svgPanel(A, verdicts, PANEL_PAD, PANEL_PAD + LEDGER_HEAD);
  const right = svgPanel(B, verdicts, PANEL_PAD + W + gap, PANEL_PAD + LEDGER_HEAD);
  const lines = [
    { text: "compared " + (r.agree.length + r.differ.length) + "/" + r.keys +
            " elements present in either side   viewport " + W + "x" + H + "   tolerance " + r.tolerance + "px" },
    { text: "agree " + r.agree.length + "   differ " + r.differ.length + "   unanswered " +
            r.unanswered.length + "   only-in-left " + r.onlyA.length + "   only-in-right " + r.onlyB.length },
    { text: "" },
  ];
  for (const d of r.differ.slice(0, 30)) {
    lines.push({ text: "DIFFER  " + d.key + "  " +
                       d.fields.map((f) => f.field + " " + f.a + "→" + f.b + " (" +
                                    (f.delta > 0 ? "+" : "") + f.delta + ")").join("  "), fill: "#d0021b" });
  }
  if (r.differ.length > 30) lines.push({ text: "… " + (r.differ.length - 30) + " further differing elements",
                                         fill: "#d0021b" });
  for (const u of r.unanswered.slice(0, 15)) {
    lines.push({ text: "ABSENT  " + u.key + "  left=" + u.a + "  right=" + u.b, fill: "#f5a623" });
  }
  for (const o of r.onlyA.slice(0, 15)) lines.push({ text: "ONLY-LEFT   " + o.key, fill: "#0b6cff" });
  for (const o of r.onlyB.slice(0, 15)) lines.push({ text: "ONLY-RIGHT  " + o.key, fill: "#0b6cff" });
  const led = svgLedger(lines, PANEL_PAD, PANEL_PAD + LEDGER_HEAD + H + 26, W * 2 + gap);
  const totalH = PANEL_PAD * 2 + LEDGER_HEAD + H + 26 + led.height;
  return { svg: svgDoc(W * 2 + gap + PANEL_PAD * 2, totalH, left.xml + "\n" + right.xml + "\n" + led.xml),
           result: r };
}

function svgDoc(w, h, body) {
  return '<?xml version="1.0" encoding="UTF-8"?>\n' +
         '<svg xmlns="http://www.w3.org/2000/svg" width="' + w + '" height="' + h + '" viewBox="0 0 ' + w +
         " " + h + '">\n<rect width="' + w + '" height="' + h + '" fill="#f7f7f8"/>\n' + body + "\n</svg>\n";
}

/* ── the Chrome side ───────────────────────────────────────────────────────────────────────────────────────
 *
 * `setViewport` overrides the LAYOUT viewport through the debugger, which is the exact pin; sizing the window
 * would leave the browser's own chrome between the number asked for and the number the page sees. The pin is
 * then VERIFIED from the collector's own reading rather than assumed, because an override that silently did
 * not apply reports a clean artifact at the wrong size. */
async function collectFromChrome(url, viewport) {
  const lock = JSON.parse(fs.readFileSync(LOCK_FILE, "utf8"));
  const port = lock.port || DEFAULT_PORT;
  const browser = await puppeteer.connect({
    browserURL: "http://127.0.0.1:" + port,
    defaultViewport: null,
    targetFilter: (t) => t.type() !== "browser",
    protocolTimeout: 300000,
  });
  try {
    const pages = await browser.pages();
    const page = pages.filter((p) => !p.url().startsWith("chrome-extension://") &&
                                     !p.url().startsWith("devtools://")).pop() || pages[0];
    check(page !== undefined, "no page to collect from — run: node testing/harness.js restart");
    if (url) await page.goto(url, { waitUntil: "load" });
    await page.setViewport({ width: viewport.width, height: viewport.height });
    const art = checkArtifactBody(await page.evaluate(collectorSource()),
                                  "the body the collector returned from the page");
    art.producer = "chrome";
    art.chromeVersion = await browser.version();
    checkArtifact(art, "the artifact Chrome just produced");
    check(art.viewport.width === viewport.width && art.viewport.height === viewport.height,
          "the viewport override did not take: asked for " + viewport.width + "x" + viewport.height +
          " and the page reports " + art.viewport.width + "x" + art.viewport.height + ". An artifact " +
          "collected at a size nobody asked for is a measurement of a layout nobody chose");
    return art;
  } finally { browser.disconnect(); }
}

/* A REAL CAPTURE OF THE REAL BROWSER — and it is a REFERENCE, not the other half of the comparison. It is what
   a person means by "show me the page", and it is deliberately unreachable from `compare`: a PNG and a box
   diagram are two kinds of artifact, and pairing them would be the constraint this file exists to keep. It is
   pinned to the same viewport as everything else so that it is a photograph OF the layout being scored rather
   than of some other one. */
async function shotFromChrome(url, viewport, outPath) {
  const lock = JSON.parse(fs.readFileSync(LOCK_FILE, "utf8"));
  const port = lock.port || DEFAULT_PORT;
  const browser = await puppeteer.connect({
    browserURL: "http://127.0.0.1:" + port,
    defaultViewport: null,
    targetFilter: (t) => t.type() !== "browser",
    protocolTimeout: 300000,
  });
  try {
    const pages = await browser.pages();
    const page = pages.filter((p) => !p.url().startsWith("chrome-extension://") &&
                                     !p.url().startsWith("devtools://")).pop() || pages[0];
    check(page !== undefined, "no page to capture — run: node testing/harness.js restart");
    if (url) await page.goto(url, { waitUntil: "load" });
    await page.setViewport({ width: viewport.width, height: viewport.height });
    const seen = await page.evaluate("[innerWidth, innerHeight]");
    check(seen[0] === viewport.width && seen[1] === viewport.height,
          "the viewport override did not take: asked for " + viewport.width + "x" + viewport.height +
          " and the page reports " + seen[0] + "x" + seen[1] + ", so this capture is of a layout nobody chose");
    await page.screenshot({ path: outPath, fullPage: false });
    return { url: page.url(), viewport: { width: seen[0], height: seen[1] } };
  } finally { browser.disconnect(); }
}

/* ── the armed control ─────────────────────────────────────────────────────────────────────────────────────
 *
 * A comparator nobody has seen REJECT anything has calibrated nothing, so `selfcheck` runs both arms and fails
 * unless each behaves: Chrome against Chrome at ONE viewport must report zero differences over a non-empty
 * population (the NEGATIVE control — a comparator that reports differences here is reporting on itself), and
 * Chrome at a SECOND viewport must be REFUSED (the POSITIVE control — the refusal is the check firing). The
 * second arm is the one that proves the viewport is load-bearing rather than recorded.
 * RETIREMENT: this control goes when the engine side exists and the same two arms run against it, at which
 * point Chrome-against-Chrome is a degenerate case of the real comparison rather than the only one available. */
async function selfcheck(url) {
  const a = await collectFromChrome(url, ENGINE_VIEWPORT);
  const b = await collectFromChrome(null, ENGINE_VIEWPORT);
  const same = compare(a, b, 0);
  check(same.keys > 0, "the self-check compared ZERO elements, so neither arm established anything — point it " +
                       "at a document with elements in it");
  check(same.differ.length === 0 && same.onlyA.length === 0 && same.onlyB.length === 0,
        "NEGATIVE CONTROL FAILED: one browser at one viewport disagreed with itself — " + same.differ.length +
        " differing, " + same.onlyA.length + "/" + same.onlyB.length + " one-sided. The comparator or the " +
        "collector is reporting on itself and no engine number taken with it means anything");
  const other = { width: 375, height: 667 };
  const c = await collectFromChrome(null, other);
  let refused = null;
  try { compare(a, c, 0); } catch (e) { refused = e.message; }
  check(refused !== null,
        "POSITIVE CONTROL FAILED: a comparison across " + ENGINE_VIEWPORT.width + "x" + ENGINE_VIEWPORT.height +
        " and " + other.width + "x" + other.height + " was ACCEPTED. The viewport is then recorded and not " +
        "enforced, and every later number is a comparison of two layouts");
  return "selfcheck PASSED\n" +
         "  negative control: chrome vs chrome @ " + ENGINE_VIEWPORT.width + "x" + ENGINE_VIEWPORT.height +
         " — " + same.agree.length + "/" + same.keys + " agree, 0 differ\n" +
         "  positive control: cross-viewport comparison REFUSED — " + refused.split("\n")[0];
}

function parseViewport(s) {
  const m = /^(\d+)x(\d+)$/.exec(s || "");
  check(m !== null, "--viewport wants WxH in CSS pixels, e.g. 1280x720; got " + s);
  return { width: Number(m[1]), height: Number(m[2]) };
}

/* ONE PASS, so a flag's VALUE can never be mistaken for a positional. The earlier spelling asked `indexOf` for
   the position of each token, which answers the FIRST occurrence — so a value equal to an earlier token was
   read as a file name. A parser that is wrong only for repeated arguments is wrong in exactly the runs nobody
   re-reads. */
const FLAGS_WITH_VALUES = new Set(["--url", "--viewport", "--tolerance"]);
function parseArgs(args) {
  const flags = Object.create(null), positional = [];
  for (let i = 0; i < args.length; i++) {
    const a = args[i];
    if (FLAGS_WITH_VALUES.has(a)) {
      check(i + 1 < args.length, a + " needs a value");
      flags[a] = args[++i];
    } else if (a.startsWith("--")) {
      fail("unknown flag " + a);
    } else {
      positional.push(a);
    }
  }
  return { flags, positional };
}

const CMDS = {
  collector: async () => collectorSource(),
  collect: async (args) => {
    const { flags, positional } = parseArgs(args);
    check(positional.length === 1,
          "usage: render_diff.js collect <out.json> [--url <u>] [--viewport 1280x720]");
    const vp = flags["--viewport"] ? parseViewport(flags["--viewport"]) : ENGINE_VIEWPORT;
    const art = await collectFromChrome(flags["--url"] || null, vp);
    fs.writeFileSync(positional[0], JSON.stringify(art, null, 1));
    return "wrote " + positional[0] + "  producer=" + art.producer + "  " + art.elementCount +
           " elements @ " + art.viewport.width + "x" + art.viewport.height + "  url=" + art.url;
  },
  compare: async (args) => {
    const { flags, positional } = parseArgs(args);
    check(positional.length === 2, "usage: render_diff.js compare <a.json> <b.json> [--tolerance <px>]");
    const tol = flags["--tolerance"] === undefined ? 0 : Number(flags["--tolerance"]);
    const A = JSON.parse(fs.readFileSync(positional[0], "utf8"));
    const B = JSON.parse(fs.readFileSync(positional[1], "utf8"));
    return report(compare(A, B, tol),
                  positional[0] + " (" + A.producer + ")", positional[1] + " (" + B.producer + ")");
  },
  selfcheck: async (args) => selfcheck(parseArgs(args).flags["--url"] || null),
  /* THE VIEWABLE HALF. `svg` and `svgdiff` are pure functions of artifacts already on disk — they connect to
     nothing and need no browser, which is what lets the ENGINE's artifact be drawn by the same renderer the
     day it exists. `shot` is the only one of the three that touches Chrome, and it produces a REFERENCE. */
  svg: async (args) => {
    const { flags, positional } = parseArgs(args);
    check(positional.length === 2, "usage: render_diff.js svg <artifact.json> <out.svg>");
    void flags;
    const art = JSON.parse(fs.readFileSync(positional[0], "utf8"));
    fs.writeFileSync(positional[1], svgOne(art));
    return "wrote " + positional[1] + "  " + art.elementCount + " elements from " + art.producer + " @ " +
           art.viewport.width + "x" + art.viewport.height;
  },
  svgdiff: async (args) => {
    const { flags, positional } = parseArgs(args);
    check(positional.length === 3, "usage: render_diff.js svgdiff <a.json> <b.json> <out.svg> [--tolerance <px>]");
    const tol = flags["--tolerance"] === undefined ? 0 : Number(flags["--tolerance"]);
    const A = JSON.parse(fs.readFileSync(positional[0], "utf8"));
    const B = JSON.parse(fs.readFileSync(positional[1], "utf8"));
    const { svg, result } = svgCompare(A, B, tol);
    fs.writeFileSync(positional[2], svg);
    return "wrote " + positional[2] + "  (left " + A.producer + ", right " + B.producer + ")\n" +
           report(result, positional[0] + " (" + A.producer + ")", positional[1] + " (" + B.producer + ")");
  },
  shot: async (args) => {
    const { flags, positional } = parseArgs(args);
    check(positional.length === 1, "usage: render_diff.js shot <out.png> [--url <u>] [--viewport 1280x720]");
    const vp = flags["--viewport"] ? parseViewport(flags["--viewport"]) : ENGINE_VIEWPORT;
    const got = await shotFromChrome(flags["--url"] || null, vp, positional[0]);
    return "wrote " + positional[0] + "  REFERENCE capture of real Chrome @ " + got.viewport.width + "x" +
           got.viewport.height + "  url=" + got.url + "\n" +
           "  this is NOT an input to `compare`: a photograph and a box diagram are two kinds of artifact, " +
           "and the comparison is between two artifacts of ONE kind.";
  },
};

async function main() {
  const [cmd, ...rest] = process.argv.slice(2);
  const fn = cmd && Object.prototype.hasOwnProperty.call(CMDS, cmd) ? CMDS[cmd] : null;
  if (!fn) {
    console.error("usage: node testing/render_diff.js <collector | collect | compare | selfcheck | " +
                  "svg | svgdiff | shot> [args\u2026]");
    process.exit(2);
  }
  try { console.log(await fn(rest)); }
  catch (e) { console.error(e.stack || e.message || e); process.exit(1); }
}

/* NAMED RESIDUAL — the artifact covers the ELEMENT TREE OF ONE DOCUMENT AND NOTHING ELSE.
 *   WHAT IS NOT COVERED: a document's `children` walk does not descend into a shadow root's tree or into a
 *     child navigable's document, so an element inside either contributes no row on either side. This is a
 *     property of the collector's four platform reads, not of the comparator, which never asks where a row
 *     came from.
 *   WHAT THE NEXT DIFF BUILDS: the key grammar gains a step for a crossing — a shadow host and a frame element
 *     each name a boundary the path descends through — and the collector follows it, so a subtree's rows are
 *     joined to the same subtree on the other side rather than to nothing.
 *   HOW ITS ABSENCE WOULD BE OBSERVED: `elementCount` on a document whose content is inside a frame or a
 *     shadow tree is a small number that does not grow with the page, and the comparison reports agreement
 *     over a population that excludes everything the page actually renders. */
/* THE WORK IS BEHIND A GUARD, AND THAT IS NOT TIDINESS. testing/harness.js calls `main()` at its top level,
   so REQUIRING it is RUNNING it — which is why this file re-reads its lock convention instead of importing it,
   and it would be the same defect to reproduce here. Guarded, the comparator and the collector source are
   reachable from a gate or a future engine-side driver without launching a browser.
   RETIREMENT: this note goes when nothing in testing/ does work at import, so a reader has no counterexample
   to re-derive the habit from. */
module.exports = { COLLECTOR, collectorSource, compare, checkArtifactBody, checkArtifact, report,
                   svgOne, svgCompare, ENGINE_VIEWPORT };

if (require.main === module) main();
