"use strict";
// viewer.js — Source code viewer with Prism.js syntax highlighting.
// Opens in a new tab from the popup's security findings panel.
// Fetches original source via background.js, beautifies with Babel,
// highlights with Prism, and scrolls to the target finding line.
// Supports cross-file navigation, click-to-definition, and focused mode
// (tree-shakes irrelevant code, showing only functions reachable from findings).

(function() {

var params = new URLSearchParams(location.search);
var tabId = parseInt(params.get("tabId"), 10) || 0;

var urlEl = document.getElementById("source-url");
var pageUrlEl = document.getElementById("page-url");
var statusEl = document.getElementById("status");
var codeEl = document.getElementById("code-output");
var preEl = document.getElementById("code-pre");
var containerEl = document.getElementById("code-container");
var pickerEl = document.getElementById("file-picker");
var focusBtnEl = document.getElementById("btn-focus");

var _currentUrl = null;
var _defMap = null;       // { name: line } for click-to-definition (fallback)
var _refMap = null;       // { line: { name: defLine } } — scope-resolved per-reference
var _funcMap = null;      // { name: { line, endLine, calls: Set } } — named only
var _allFuncRanges = null; // [{ line, endLine, calls }] — ALL functions (named + anonymous)
var _ast = null;           // Saved Babel AST from buildCodeGraph for reuse

// Focus mode state
var _focusMode = true;    // default: focused
var _fullCode = null;     // cached beautified full source
var _focusedCode = null;  // cached focused source
var _lineRemap = null;    // Map<focusedLine, originalBeautifiedLine>
var _mappedFindings = null;
var _mappedTarget = null;
var _hasFocusableFindings = false;

function showMessage(text, isError) {
  containerEl.innerHTML = '<div class="viewer-message' + (isError ? ' viewer-error' : '') + '">' +
    escHtml(text) + '</div>';
}

function escHtml(s) {
  var d = document.createElement("div");
  d.textContent = s;
  return d.innerHTML;
}

// ─── Minifier Pattern Decompression (display-only) ──────────────────────────

var _bt = BabelBundle.t;

function _expandMinifiedStatement(stmt) {
  if (!stmt || stmt.type !== "ExpressionStatement") return [stmt];
  var expr = stmt.expression;
  if (expr.type === "SequenceExpression") {
    return expr.expressions.map(function(e) { return _bt.expressionStatement(e); });
  }
  if (expr.type === "LogicalExpression" && expr.operator === "&&") {
    return [_bt.ifStatement(expr.left, _exprToBlock(expr.right))];
  }
  if (expr.type === "LogicalExpression" && expr.operator === "||") {
    return [_bt.ifStatement(_bt.unaryExpression("!", expr.left), _exprToBlock(expr.right))];
  }
  if (expr.type === "ConditionalExpression") {
    return [_bt.ifStatement(expr.test, _exprToBlock(expr.consequent), _exprToBlock(expr.alternate))];
  }
  return [stmt];
}

function _exprToBlock(expr) {
  var stmts = expr.type === "SequenceExpression"
    ? expr.expressions.map(function(e) { return _bt.expressionStatement(e); })
    : [_bt.expressionStatement(expr)];
  return _bt.blockStatement(stmts);
}

// Walk an AST and expand minifier patterns in all statement bodies.
// Mutates in-place — call on the parsed AST before generate().
function _decompressAST(ast) {
  var stack = [ast];
  while (stack.length > 0) {
    var cur = stack.pop();
    if (!cur || typeof cur !== "object") continue;
    if (Array.isArray(cur.body)) {
      var newBody = [];
      for (var i = 0; i < cur.body.length; i++) {
        var expanded = _expandMinifiedStatement(cur.body[i]);
        for (var j = 0; j < expanded.length; j++) newBody.push(expanded[j]);
      }
      cur.body = newBody;
    }
    var keys = Object.keys(cur);
    for (var k = 0; k < keys.length; k++) {
      if (keys[k] === "type" || keys[k] === "start" || keys[k] === "end" || keys[k] === "loc") continue;
      var child = cur[keys[k]];
      if (child && typeof child === "object") {
        if (Array.isArray(child)) {
          for (var c = 0; c < child.length; c++) {
            if (child[c] && typeof child[c] === "object" && child[c].type) stack.push(child[c]);
          }
        } else if (child.type) {
          stack.push(child);
        }
      }
    }
  }
}

// ─── Beautify ────────────────────────────────────────────────────────────────

function beautify(rawCode) {
  console.log("[viewer:beautify] Input length=%d, first 200 chars: %s", rawCode.length, JSON.stringify(rawCode.substring(0, 200)));
  console.log("[viewer:beautify] Char codes at start: %s", Array.from(rawCode.substring(0, 10)).map(function(c) { return c.charCodeAt(0); }).join(", "));
  try {
    var ast;
    var parseOpts = { plugins: ["jsx"], errorRecovery: true, sourceFilename: "input.js" };
    try {
      ast = BabelBundle.parse(rawCode, Object.assign({ sourceType: "module" }, parseOpts));
    } catch (modErr) {
      console.log("[viewer:beautify] Module parse failed: %s", modErr.message);
      ast = BabelBundle.parse(rawCode, Object.assign({ sourceType: "script" }, parseOpts));
    }

    // Expand minifier patterns (&&, ||, ternary, comma sequences) for readability
    _decompressAST(ast);

    var result = BabelBundle.generate(ast, {
      compact: false,
      concise: false,
      retainLines: false,
      sourceMaps: true,
      sourceFileName: "input.js",
    }, { "input.js": rawCode });

    _vlqState = [0, 0, 0, 0, 0];
    var lineMap = {};      // origLine → first genLine (line-only lookups)
    var colMap = {};       // origLine → [{origCol, genLine}] (column-precise lookups)
    if (result.map && result.map.mappings) {
      var mappings = result.map.mappings;
      var genLine = 0;
      for (var i = 0; i < mappings.length; i++) {
        var ch = mappings.charAt(i);
        if (ch === ";") {
          genLine++;
          _vlqState[0] = 0;
          continue;
        }
        if (ch === ",") continue;
        var decoded = decodeVLQSegment(mappings, i);
        if (decoded && decoded.originalLine != null) {
          var origLine = decoded.originalLine + 1;
          var origCol = decoded.originalColumn || 0;
          if (!lineMap[origLine] || lineMap[origLine] > genLine + 1) {
            lineMap[origLine] = genLine + 1;
          }
          if (!colMap[origLine]) colMap[origLine] = [];
          colMap[origLine].push({ origCol: origCol, genLine: genLine + 1 });
        }
        while (i < mappings.length && mappings.charAt(i) !== "," && mappings.charAt(i) !== ";") i++;
        i--;
      }
    }

    return { code: result.code, lineMap: lineMap, colMap: colMap };
  } catch (e) {
    console.debug("[viewer] Beautify failed:", e.message);
    return { code: rawCode, lineMap: null };
  }
}

// ─── VLQ Decoder ─────────────────────────────────────────────────────────────

var VLQ_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
var _vlqLookup = null;
function decodeVLQ(mappings, pos) {
  if (!_vlqLookup) {
    _vlqLookup = {};
    for (var i = 0; i < VLQ_CHARS.length; i++) _vlqLookup[VLQ_CHARS.charAt(i)] = i;
  }
  var result = 0, shift = 0, startPos = pos;
  while (pos < mappings.length) {
    var ch = mappings.charAt(pos);
    var val = _vlqLookup[ch];
    if (val === undefined) break;
    pos++;
    result += (val & 31) << shift;
    shift += 5;
    if ((val & 32) === 0) break;
  }
  var value = (result & 1) ? -(result >> 1) : (result >> 1);
  return { value: value, length: pos - startPos };
}

var _vlqState = [0, 0, 0, 0, 0];
function decodeVLQSegment(mappings, pos) {
  var d0 = decodeVLQ(mappings, pos);
  if (!d0 || d0.length === 0) return null;
  pos += d0.length;
  _vlqState[0] += d0.value;

  if (pos >= mappings.length || mappings.charAt(pos) === "," || mappings.charAt(pos) === ";") {
    return { originalLine: null };
  }

  var d1 = decodeVLQ(mappings, pos);
  if (!d1) return null;
  pos += d1.length;
  _vlqState[1] += d1.value;

  var d2 = decodeVLQ(mappings, pos);
  if (!d2) return null;
  pos += d2.length;
  _vlqState[2] += d2.value;

  var d3 = decodeVLQ(mappings, pos);
  if (!d3) return null;
  pos += d3.length;
  _vlqState[3] += d3.value;

  return { originalLine: _vlqState[2], originalColumn: _vlqState[3] };
}

// ─── Line Mapping & Findings ─────────────────────────────────────────────────

function mapLine(originalLine, lineMap, colMap, originalCol) {
  if (!lineMap) return originalLine;
  // Column-precise lookup for minified code (one source line → many beautified lines)
  if (originalCol != null && colMap && colMap[originalLine]) {
    var entries = colMap[originalLine];
    var best = null;
    for (var ei = 0; ei < entries.length; ei++) {
      if (entries[ei].origCol <= originalCol) {
        if (!best || entries[ei].origCol > best.origCol) {
          best = entries[ei];
        }
      }
    }
    if (best) {
      console.log("[viewer:mapLine] col-precise: orig %d:%d → beautified %d (matched col %d)", originalLine, originalCol, best.genLine, best.origCol);
      return best.genLine;
    }
  }
  if (lineMap[originalLine]) {
    console.log("[viewer:mapLine] exact: orig %d → beautified %d", originalLine, lineMap[originalLine]);
    return lineMap[originalLine];
  }
  for (var l = originalLine; l > 0; l--) {
    if (lineMap[l]) {
      console.log("[viewer:mapLine] fallback: orig %d → nearest orig %d → beautified %d (gap=%d)", originalLine, l, lineMap[l], originalLine - l);
      return lineMap[l];
    }
  }
  console.warn("[viewer:mapLine] no mapping found for orig %d", originalLine);
  return originalLine;
}

function collectFindingLines(findings) {
  var lines = [];
  if (!findings) return lines;
  for (var fi = 0; fi < findings.length; fi++) {
    var f = findings[fi];
    var sinks = f.securitySinks || [];
    var patterns = f.dangerousPatterns || [];
    for (var si = 0; si < sinks.length; si++) {
      if (sinks[si].location) {
        lines.push({ line: sinks[si].location.line, col: sinks[si].location.column, severity: sinks[si].severity || "high" });
      }
    }
    for (var di = 0; di < patterns.length; di++) {
      if (patterns[di].location) {
        lines.push({ line: patterns[di].location.line, col: patterns[di].location.column, severity: patterns[di].severity || "medium" });
      }
    }
  }
  return lines;
}

function markFindingLinesInGutter(mappedLines, remap) {
  var rows = preEl.querySelector(".line-numbers-rows");
  if (!rows) return;
  var spans = rows.children;
  for (var i = 0; i < mappedLines.length; i++) {
    var targetLine = mappedLines[i].line;
    // In focused mode, reverse-map the beautified line to the focused line
    var focusedLine = targetLine;
    if (remap) {
      focusedLine = null;
      remap.forEach(function(origLine, fLine) {
        if (origLine === targetLine) focusedLine = fLine;
      });
      if (focusedLine == null) continue;
    }
    var lineIdx = focusedLine - 1;
    if (lineIdx >= 0 && lineIdx < spans.length) {
      var cls = mappedLines[i].severity === "high" ? "finding-line-high" : "finding-line";
      spans[lineIdx].classList.add(cls);
    }
  }
}

function addFindingOverlays(highlightLines, severities) {
  // Remove old overlays
  var old = preEl.querySelectorAll(".finding-overlay");
  for (var oi = 0; oi < old.length; oi++) old[oi].remove();

  if (!highlightLines.length) return;
  preEl.style.position = "relative";
  var rows = preEl.querySelector(".line-numbers-rows");
  var fallbackHeight = parseFloat(getComputedStyle(codeEl).lineHeight) || 20;
  var padTop = parseFloat(getComputedStyle(preEl).paddingTop) || 0;
  for (var i = 0; i < highlightLines.length; i++) {
    var div = document.createElement("div");
    div.className = "finding-overlay " + (severities[i] === "high" ? "finding-overlay-high" : "finding-overlay-medium");
    var gs = rows && rows.children[highlightLines[i] - 1];
    if (gs) {
      var preRect = preEl.getBoundingClientRect();
      var gsRect = gs.getBoundingClientRect();
      div.style.top = (gsRect.top - preRect.top) + "px";
      div.style.height = gsRect.height + "px";
    } else {
      div.style.top = (padTop + (highlightLines[i] - 1) * fallbackHeight) + "px";
      div.style.height = fallbackHeight + "px";
    }
    preEl.appendChild(div);
  }
}

var _defHighlightEl = null;

function scrollToLine(line, highlight) {
  console.log("[viewer:scrollToLine] Requested line=%s highlight=%s", line, !!highlight);
  if (!line || line < 1) {
    console.warn("[viewer:scrollToLine] Invalid line, skipping");
    return;
  }
  requestAnimationFrame(function() {
    var rows = preEl.querySelector(".line-numbers-rows");
    var gutterSpan = rows && rows.children[line - 1];
    var lineHeight, offset;
    if (gutterSpan) {
      // Use actual rendered gutter position for accurate scrolling
      var preRect = preEl.getBoundingClientRect();
      var gutterRect = gutterSpan.getBoundingClientRect();
      offset = gutterRect.top - preRect.top;
      lineHeight = gutterRect.height;
    } else {
      lineHeight = parseFloat(getComputedStyle(codeEl).lineHeight) || 20;
      offset = (line - 1) * lineHeight;
    }
    var containerHeight = containerEl.clientHeight;
    var scrollPos = Math.max(0, offset - containerHeight / 3);
    console.log("[viewer:scrollToLine] lineHeight=%d, offset=%d, containerHeight=%d, scrollTop=%d", lineHeight, offset, containerHeight, scrollPos);
    containerEl.scrollTop = scrollPos;

    if (highlight) {
      // Clear previous gutter highlight
      var prevGutter = preEl.querySelectorAll(".def-target");
      for (var j = 0; j < prevGutter.length; j++) prevGutter[j].classList.remove("def-target");

      // Highlight gutter
      if (gutterSpan) {
        gutterSpan.classList.add("def-target");
      }

      // Position overlay highlight using gutter span's actual rendered position
      if (!_defHighlightEl) {
        _defHighlightEl = document.createElement("div");
        _defHighlightEl.id = "def-highlight";
        preEl.style.position = "relative";
        preEl.appendChild(_defHighlightEl);
      }
      _defHighlightEl.style.top = offset + "px";
      _defHighlightEl.style.height = lineHeight + "px";
      _defHighlightEl.style.display = "block";
    }
  });
}

// ─── Code Graph ──────────────────────────────────────────────────────────────

// Build code graph from the beautified AST.
// Delegates to buildDefinitionMap() from ast.js which uses the same Babel
// scope system, type tracking, and property resolution as the security analysis.
function buildCodeGraph(beautifiedCode) {
  var result = buildDefinitionMap(beautifiedCode);
  _funcMap = result.funcMap;
  _defMap = result.defMap;
  _refMap = result.refMap;
  _allFuncRanges = result.allFuncRanges;
  _ast = result.ast;
  console.log("[viewer:buildCodeGraph] defMap:%d refMap:%d funcMap:%d ranges:%d propDefs:%d",
    Object.keys(result.defMap).length, Object.keys(result.refMap).length,
    Object.keys(result.funcMap).length, result.allFuncRanges.length,
    Object.keys(result.propDefs).length);
}

// ─── Reachability (Focus Mode) ───────────────────────────────────────────────

// Find all function ranges reachable from security findings.
// Uses _allFuncRanges for containment (any function, named or not),
// then BFS through _funcMap (named only) for call-graph expansion.
// Returns array of [startLine, endLine] ranges, or null.
function buildRelevantRanges(mappedFindings) {
  if (!_allFuncRanges || !mappedFindings || !mappedFindings.length) return null;

  // Seed: find the innermost function containing each finding line
  var seedRanges = []; // direct ranges from containment
  var seedNames = new Set(); // named functions for BFS expansion
  for (var fi = 0; fi < mappedFindings.length; fi++) {
    var fLine = mappedFindings[fi].line;
    // Find innermost (smallest range) function containing this line
    var best = null;
    for (var ri = 0; ri < _allFuncRanges.length; ri++) {
      var r = _allFuncRanges[ri];
      if (fLine >= r.line && fLine <= r.endLine) {
        if (!best || (r.endLine - r.line) < (best.endLine - best.line)) {
          best = r;
        }
      }
    }
    if (best) {
      seedRanges.push([best.line, best.endLine]);
      // If this range also has named calls, queue them for BFS
      if (best.calls) {
        best.calls.forEach(function(c) { seedNames.add(c); });
      }
    }
  }

  if (seedRanges.length === 0) return null;

  // BFS through named call graph
  var visitedNames = new Set();
  var queue = [];
  seedNames.forEach(function(s) {
    if (_funcMap[s] && !visitedNames.has(s)) {
      visitedNames.add(s);
      queue.push({ name: s, depth: 0 });
    }
  });
  while (queue.length > 0) {
    var item = queue.shift();
    if (item.depth >= 10) continue;
    var func = _funcMap[item.name];
    if (!func || !func.calls) continue;
    func.calls.forEach(function(callee) {
      if (!visitedNames.has(callee) && _funcMap[callee]) {
        visitedNames.add(callee);
        queue.push({ name: callee, depth: item.depth + 1 });
      }
    });
  }

  // Collect all ranges: seeds + BFS-reached named functions
  var ranges = seedRanges.slice();
  visitedNames.forEach(function(name) {
    var fn = _funcMap[name];
    if (fn) ranges.push([fn.line, fn.endLine]);
  });

  return ranges;
}

// Build focused code: only relevant function lines, with separators.
// Returns { code, lineRemap: Map<focusedLine, beautifiedLine> }
function buildFocusedCode(beautifiedCode, ranges) {
  if (!ranges || ranges.length === 0) return null;

  // Sort by start line
  ranges.sort(function(a, b) { return a[0] - b[0]; });

  // Merge overlapping/adjacent ranges (1-line gap tolerance)
  var merged = [ranges[0]];
  for (var ri = 1; ri < ranges.length; ri++) {
    var last = merged[merged.length - 1];
    if (ranges[ri][0] <= last[1] + 2) {
      last[1] = Math.max(last[1], ranges[ri][1]);
    } else {
      merged.push(ranges[ri]);
    }
  }

  // Extract lines
  var allLines = beautifiedCode.split("\n");
  var focusedLines = [];
  var lineRemap = new Map(); // focusedLineNum (1-based) → beautifiedLineNum (1-based)

  for (var mi = 0; mi < merged.length; mi++) {
    var start = merged[mi][0]; // 1-based
    var end = Math.min(merged[mi][1], allLines.length);

    // Insert separator before this range (not before the first)
    if (mi > 0) {
      var prevEnd = merged[mi - 1][1];
      var gapSize = start - prevEnd - 1;
      focusedLines.push("// \u00b7\u00b7\u00b7 " + gapSize + " lines hidden \u00b7\u00b7\u00b7");
      lineRemap.set(focusedLines.length, -1); // separator marker
    }

    // Add function lines
    for (var li = start; li <= end; li++) {
      focusedLines.push(allLines[li - 1]); // allLines is 0-indexed
      lineRemap.set(focusedLines.length, li); // focusedLines.length is current 1-based line
    }
  }

  // Add trailing separator if there's code after the last range
  if (merged.length > 0) {
    var lastEnd = merged[merged.length - 1][1];
    if (lastEnd < allLines.length) {
      var trailGap = allLines.length - lastEnd;
      focusedLines.push("// \u00b7\u00b7\u00b7 " + trailGap + " lines hidden \u00b7\u00b7\u00b7");
      lineRemap.set(focusedLines.length, -1);
    }
    // Leading separator
    if (merged[0][0] > 1) {
      var leadGap = merged[0][0] - 1;
      focusedLines.unshift("// \u00b7\u00b7\u00b7 " + leadGap + " lines hidden \u00b7\u00b7\u00b7");
      // Rebuild lineRemap since we shifted everything by 1
      var newRemap = new Map();
      lineRemap.forEach(function(v, k) { newRemap.set(k + 1, v); });
      newRemap.set(1, -1); // leading separator
      lineRemap = newRemap;
    }
  }

  return {
    code: focusedLines.join("\n"),
    lineRemap: lineRemap,
  };
}

// ─── Render ──────────────────────────────────────────────────────────────────

function renderCode(code, remap) {
  console.log("[viewer:renderCode] code length=%d, remap=%s, _mappedTarget=%s", code.length, !!remap, _mappedTarget);
  // Restore pre/code structure (showMessage may have replaced container)
  containerEl.innerHTML = "";
  containerEl.appendChild(preEl);

  // Set line highlight data
  var highlightLines = [];
  var highlightSeverities = [];
  if (_mappedFindings) {
    for (var i = 0; i < _mappedFindings.length; i++) {
      var targetLine = _mappedFindings[i].line;
      if (remap) {
        // Find the focused line for this beautified line
        var found = null;
        remap.forEach(function(origLine, fLine) {
          if (origLine === targetLine) found = fLine;
        });
        if (found) {
          highlightLines.push(found);
          highlightSeverities.push(_mappedFindings[i].severity);
        } else {
          console.log("[viewer:renderCode] Finding at beautified %d NOT in focused view", targetLine);
        }
      } else {
        highlightLines.push(targetLine);
        highlightSeverities.push(_mappedFindings[i].severity);
      }
    }
  }
  if (_mappedTarget) {
    var scrollTarget = _mappedTarget;
    if (remap) {
      var mapped = null;
      remap.forEach(function(origLine, fLine) {
        if (origLine === _mappedTarget) mapped = fLine;
      });
      if (mapped) {
        scrollTarget = mapped;
      } else if (highlightLines.length > 0) {
        // Target not in focused view — scroll to first finding instead
        scrollTarget = highlightLines[0];
        console.log("[viewer:renderCode] Target %d not in focused view, using first highlight line %d", _mappedTarget, scrollTarget);
      }
    }
    console.log("[viewer:renderCode] scrollTarget: beautified %d → %s %d", _mappedTarget, remap ? "focused" : "beautified", scrollTarget);
  }
  console.log("[viewer:renderCode] highlightLines (%d): %s", highlightLines.length, highlightLines.join(","));
  // Log what's actually at the scroll target line
  var codeLines = code.split("\n");
  if (scrollTarget && scrollTarget > 0 && scrollTarget <= codeLines.length) {
    console.log("[viewer:renderCode] Content at scroll target line %d: %s", scrollTarget, JSON.stringify(codeLines[scrollTarget - 1].substring(0, 120)));
  }
  // Log content at each highlight line
  for (var _dli = 0; _dli < Math.min(highlightLines.length, 10); _dli++) {
    var _hl = highlightLines[_dli];
    if (_hl > 0 && _hl <= codeLines.length) {
      console.log("[viewer:renderCode] Highlight line %d: %s", _hl, JSON.stringify(codeLines[_hl - 1].substring(0, 120)));
    }
  }

  preEl.removeAttribute("data-line");

  codeEl.textContent = code;
  Prism.highlightElement(codeEl);

  // Fix line numbers in focused mode
  if (remap) {
    fixLineNumbers(remap);
  }

  // Mark finding lines in gutter + background overlays
  markFindingLinesInGutter(_mappedFindings || [], remap);
  addFindingOverlays(highlightLines, highlightSeverities);

  // Attach definition links
  attachDefinitionLinks();

  // Scroll
  scrollToLine(scrollTarget || _mappedTarget);
}

function fixLineNumbers(remap) {
  var rows = preEl.querySelector(".line-numbers-rows");
  if (!rows) return;
  var spans = rows.children;
  for (var i = 0; i < spans.length; i++) {
    var lineNum = i + 1;
    var origLine = remap.get(lineNum);
    if (origLine === -1) {
      // Separator line
      spans[i].classList.add("hidden-separator");
      spans[i].setAttribute("data-line", "\u00b7\u00b7\u00b7");
    } else if (origLine) {
      spans[i].setAttribute("data-line", String(origLine));
    }
  }
}

// ─── Clickable Function Tokens ───────────────────────────────────────────────

function _resolveDefLine(name, renderedLine) {
  // Convert rendered line to beautified line (in focused mode, lines are renumbered)
  var line = renderedLine;
  if (_focusMode && _lineRemap && _lineRemap.has(renderedLine)) {
    line = _lineRemap.get(renderedLine);
  }
  // Scope-resolved lookup — only trust Babel's scope analysis
  if (_refMap) {
    if (_refMap[line] && _refMap[line][name]) return _refMap[line][name];
    // Scope analysis ran but no binding for this name on this line.
    // It's a declaration, property access, or unbound global —
    // flat defMap would link to a random same-named definition.
    return 0;
  }
  // No scope analysis available — fall back to flat defMap
  if (_defMap && _defMap[name]) return _defMap[name];
  return 0;
}

// Build a WeakMap mapping every DOM node (text + element) inside codeEl
// to its starting line number. Walks in document order counting \n.
var _nodeLineMap = null;
function _tagTokenLines() {
  _nodeLineMap = new WeakMap();
  var line = 1;
  var walker = document.createTreeWalker(codeEl, NodeFilter.SHOW_ALL, null);
  var node;
  while ((node = walker.nextNode())) {
    if (node.nodeType === 3) { // Text node
      _nodeLineMap.set(node, line);
      var text = node.nodeValue;
      for (var j = 0; j < text.length; j++) {
        if (text.charCodeAt(j) === 10) line++;
      }
    } else if (node.nodeType === 1) { // Element node
      _nodeLineMap.set(node, line);
    }
  }
}

function attachDefinitionLinks() {
  // Tag all tokens with line numbers for scope-aware resolution
  _tagTokenLines();

  // 1. Link Prism .token.function spans (identifiers before parens)
  var tokens = codeEl.querySelectorAll("span.token.function");
  var localCount = 0, crossCount = 0, missCount = 0;
  for (var i = 0; i < tokens.length; i++) {
    var span = tokens[i];
    var name = span.textContent;
    var tokenLine = (_nodeLineMap && _nodeLineMap.get(span)) || 0;
    var resolved = _resolveDefLine(name, tokenLine);
    if (resolved) {
      localCount++;
      span.classList.add("def-local");
      span.dataset.defLine = resolved;
      span.addEventListener("click", onLocalDefClick);
    } else if (name.length > 1) {
      crossCount++;
      span.classList.add("def-cross");
      span.dataset.defName = name;
      span.addEventListener("click", onCrossDefClick);
    } else {
      missCount++;
    }
  }

  // 2. Scan text nodes for bare identifiers resolvable via scope
  //    (Prism doesn't wrap non-call references like `l` in `addEventListener("message", l)`)
  {
    // Collect all names that _refMap knows about (scope-resolved references)
    var scanNames = {};
    if (_defMap) { for (var dn in _defMap) scanNames[dn] = 1; }
    if (_refMap) { for (var rl in _refMap) { for (var rn in _refMap[rl]) scanNames[rn] = 1; } }
    var scanNameList = Object.keys(scanNames);
    if (scanNameList.length > 0) {
      var pattern = new RegExp("\\b(" + scanNameList.map(function(n) { return n.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"); }).join("|") + ")\\b", "g");
      var walker = document.createTreeWalker(codeEl, NodeFilter.SHOW_TEXT, null);
      var textNodes = [];
      var node;
      while ((node = walker.nextNode())) textNodes.push(node);
      for (var ti = 0; ti < textNodes.length; ti++) {
        var tNode = textNodes[ti];
        // Skip if already inside a def-local/def-cross span
        if (tNode.parentElement && (tNode.parentElement.classList.contains("def-local") || tNode.parentElement.classList.contains("def-cross"))) continue;
        var text = tNode.nodeValue;
        if (!pattern.test(text)) continue;
        pattern.lastIndex = 0;
        // Look up the text node's line from the WeakMap built by _tagTokenLines
        var parentLine = (_nodeLineMap && _nodeLineMap.get(tNode)) || 0;
        var frag = document.createDocumentFragment();
        var lastIdx = 0;
        var match;
        while ((match = pattern.exec(text)) !== null) {
          var matchDefLine = _resolveDefLine(match[1], parentLine);
          if (!matchDefLine) continue; // no scope-resolved binding — skip
          if (match.index > lastIdx) frag.appendChild(document.createTextNode(text.slice(lastIdx, match.index)));
          var refSpan = document.createElement("span");
          refSpan.className = "token function def-local";
          refSpan.textContent = match[1];
          refSpan.dataset.defLine = matchDefLine;
          refSpan.addEventListener("click", onLocalDefClick);
          frag.appendChild(refSpan);
          localCount++;
          lastIdx = pattern.lastIndex;
        }
        if (lastIdx < text.length) frag.appendChild(document.createTextNode(text.slice(lastIdx)));
        if (lastIdx > 0) tNode.parentNode.replaceChild(frag, tNode);
      }
    }
  }
}

function onLocalDefClick(e) {
  e.stopPropagation();
  var clickedName = e.currentTarget.textContent;
  var origLine = parseInt(e.currentTarget.dataset.defLine, 10);
  console.log("[viewer:onLocalDefClick] Clicked '%s', dataset.defLine=%s, parsed origLine=%d", clickedName, e.currentTarget.dataset.defLine, origLine);
  console.log("[viewer:onLocalDefClick] _focusMode=%s, _lineRemap=%s", _focusMode, !!_lineRemap);
  if (!origLine) {
    console.warn("[viewer:onLocalDefClick] No origLine — aborting");
    return;
  }

  // In focused mode, reverse-map beautified line → focused line
  if (_focusMode && _lineRemap) {
    var focusedLine = null;
    _lineRemap.forEach(function(oLine, fLine) {
      if (oLine === origLine) focusedLine = fLine;
    });
    console.log("[viewer:onLocalDefClick] Focus remap: origLine %d → focusedLine %s (remap size=%d)", origLine, focusedLine, _lineRemap.size);
    if (focusedLine) {
      scrollToLine(focusedLine, true);
      return;
    }
    // Definition not in focused view — switch to full, then scroll
    console.log("[viewer:onLocalDefClick] Def not in focused view, switching to full");
    _focusMode = false;
    updateFocusButton();
    renderCode(_fullCode, null);
    scrollToLine(origLine, true);
    return;
  }

  console.log("[viewer:onLocalDefClick] Full mode, scrolling to line %d", origLine);
  scrollToLine(origLine, true);
}

function onCrossDefClick(e) {
  e.stopPropagation();
  var span = e.currentTarget;
  var name = span.dataset.defName;
  if (!name) return;
  span.classList.add("def-searching");
  chrome.runtime.sendMessage({
    type: "FIND_DEFINITION",
    tabId: tabId,
    name: name,
    excludeUrl: _currentUrl,
  }, function(result) {
    span.classList.remove("def-searching");
    if (result && result.sourceUrl) {
      loadScript(result.sourceUrl, result.line || 0);
    }
  });
}

// ─── File Picker ─────────────────────────────────────────────────────────────

function initFilePicker() {
  chrome.runtime.sendMessage({
    type: "GET_TAB_SCRIPTS",
    tabId: tabId,
  }, function(scripts) {
    if (!scripts || !scripts.length) {
      pickerEl.style.display = "none";
      return;
    }
    pickerEl.innerHTML = "";
    for (var i = 0; i < scripts.length; i++) {
      var opt = document.createElement("option");
      opt.value = scripts[i];
      opt.textContent = scripts[i].split("/").pop().split("?")[0] || scripts[i];
      opt.title = scripts[i];
      if (scripts[i] === _currentUrl) opt.selected = true;
      pickerEl.appendChild(opt);
    }
    if (scripts.length <= 1) {
      pickerEl.style.display = "none";
    }
  });
}

pickerEl.addEventListener("change", function() {
  var url = pickerEl.value;
  if (url && url !== _currentUrl) {
    loadScript(url, 0);
  }
});

// ─── Focus Toggle ────────────────────────────────────────────────────────────

function updateFocusButton() {
  if (!_hasFocusableFindings) {
    focusBtnEl.disabled = true;
    focusBtnEl.textContent = "Focus";
    focusBtnEl.classList.remove("active");
    return;
  }
  focusBtnEl.disabled = false;
  if (_focusMode) {
    focusBtnEl.textContent = "Focus";
    focusBtnEl.classList.add("active");
  } else {
    focusBtnEl.textContent = "Full";
    focusBtnEl.classList.remove("active");
  }
}

focusBtnEl.addEventListener("click", function() {
  if (!_hasFocusableFindings) return;
  _focusMode = !_focusMode;
  updateFocusButton();
  if (_focusMode && _focusedCode) {
    renderCode(_focusedCode, _lineRemap);
  } else if (_fullCode) {
    renderCode(_fullCode, null);
  }
});

// ─── Main Load ───────────────────────────────────────────────────────────────

function loadScript(scriptUrl, targetLine) {
  _currentUrl = scriptUrl;
  _defMap = null;
  _refMap = null;
  _funcMap = null;
  _allFuncRanges = null;
  _ast = null;
  _fullCode = null;
  _focusedCode = null;
  _lineRemap = null;
  _mappedFindings = null;
  _mappedTarget = null;
  _hasFocusableFindings = false;

  // Update UI
  urlEl.textContent = scriptUrl;
  document.title = scriptUrl.split("/").pop() || "Source Viewer";
  history.replaceState(null, "",
    "viewer.html?sourceUrl=" + encodeURIComponent(scriptUrl) +
    "&line=" + (targetLine || 0) + "&tabId=" + tabId);

  if (pickerEl.value !== scriptUrl) {
    pickerEl.value = scriptUrl;
  }

  showMessage("Loading source...");
  statusEl.textContent = "";
  updateFocusButton();

  chrome.runtime.sendMessage({
    type: "GET_SCRIPT_SOURCE",
    tabId: tabId,
    scriptUrl: scriptUrl,
  }, function(response) {
    if (chrome.runtime.lastError) {
      showMessage("Error: " + chrome.runtime.lastError.message, true);
      return;
    }
    if (!response) {
      showMessage("No response from background.", true);
      return;
    }
    if (response.error) {
      showMessage("Error: " + response.error, true);
      return;
    }
    if (!response.code) {
      showMessage("Empty source.", true);
      return;
    }

    var rawCode = response.code;
    var findings = response.findings || [];
    var findingLines = collectFindingLines(findings);

    // Show page context in toolbar
    if (response.pageUrl && response.pageUrl !== scriptUrl) {
      var pageName = response.pageUrl.split("/").pop().split("?")[0] || response.pageUrl;
      pageUrlEl.textContent = "in " + pageName;
      pageUrlEl.title = response.pageUrl;
    } else {
      pageUrlEl.textContent = "";
      pageUrlEl.title = "";
    }

    console.log("[viewer:loadScript] Raw code length=%d, targetLine=%d, findings count=%d", rawCode.length, targetLine, findingLines.length);
    console.log("[viewer:loadScript] Finding lines (original):", findingLines.map(function(f) { return f.line + ":" + (f.col != null ? f.col : "?") + "(" + f.severity + ")"; }).join(", "));

    statusEl.textContent = rawCode.length.toLocaleString() + " chars";
    if (findingLines.length > 0) {
      var highCount = findingLines.filter(function(f) { return f.severity === "high"; }).length;
      statusEl.innerHTML += ' <span class="badge badge-count">' + findingLines.length + ' findings</span>';
      if (highCount > 0) {
        statusEl.innerHTML += ' <span class="badge badge-high">' + highCount + ' high</span>';
      }
    }

    // Beautify
    var result = beautify(rawCode);
    var beautifiedCode = result.code;
    var lineMap = result.lineMap;
    var colMap = result.colMap;
    _fullCode = beautifiedCode;

    // Map finding lines to beautified positions (column-aware for minified code)
    console.log("[viewer:loadScript] lineMap entries: %d, colMap entries: %d", Object.keys(lineMap || {}).length, Object.keys(colMap || {}).length);
    _mappedTarget = mapLine(targetLine, lineMap, colMap);
    _mappedFindings = findingLines.map(function(f) {
      return { line: mapLine(f.line, lineMap, colMap, f.col), severity: f.severity };
    });
    console.log("[viewer:loadScript] Mapped target: %d → %d", targetLine, _mappedTarget);
    console.log("[viewer:loadScript] Mapped findings:", _mappedFindings.map(function(f) { return f.line; }).join(", "));

    // Build code graph (definitions + call references)
    buildCodeGraph(beautifiedCode);

    // Build focused view
    var relevantRanges = buildRelevantRanges(_mappedFindings);
    if (relevantRanges && relevantRanges.length > 0) {
      var focused = buildFocusedCode(beautifiedCode, relevantRanges);
      if (focused) {
        _focusedCode = focused.code;
        _lineRemap = focused.lineRemap;
        _hasFocusableFindings = true;
      }
    }

    updateFocusButton();

    // Render
    if (_focusMode && _hasFocusableFindings && _focusedCode) {
      renderCode(_focusedCode, _lineRemap);
    } else {
      renderCode(beautifiedCode, null);
    }
  });
}

// ─── Init ────────────────────────────────────────────────────────────────────

var initialUrl = params.get("sourceUrl");
var initialLine = parseInt(params.get("line"), 10) || 0;

if (!initialUrl) {
  urlEl.textContent = "(no source URL)";
  showMessage("No source URL provided.");
} else {
  loadScript(initialUrl, initialLine);
  initFilePicker();
}

})();
