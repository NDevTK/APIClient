#!/usr/bin/env node
/* The call-graph recursion census: every cycle of direct calls and tail calls in a linked native engine binary.
 * A cycle is C-stack recursion, which a flow cannot park inside and whose depth an input can grow until the stack
 * overflows; the standing target is zero. The graph comes from the binary's own disassembly, so it is what
 * compiled, after inlining. Calls through a function pointer do not appear, so the count is a floor.
 *
 *   node engine/recgate.mjs <native binary> [--list] [--why=NAME]
 *
 * --why=NAME prints the shortest call chain from NAME back to itself, which names the edges to cut.
 *
 * Exit 0: no cycle. 1: cycles found, each printed with its area and members. 2: the census could not be taken
 * (no binary, no objdump, or its own control failed), so nothing was judged. */

import { execFileSync, spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { createInterface } from 'node:readline';

const argv = process.argv.slice(2);
const bin = argv.find((a) => !a.startsWith('--'));
const list = argv.includes('--list');
const why = (argv.find((a) => a.startsWith('--why=')) || '').slice('--why='.length);
const die = (msg) => { console.error('[recgate] ' + msg); process.exit(2); };

/* Strongly connected components of `edges` (Map name -> Set of names) that are cycles: more than one member, or
   one member that calls itself. Iterative Tarjan, so the census has no recursion of its own. */
function cycles(edges) {
  const index = new Map(), low = new Map(), on = new Set(), st = [], out = [];
  let next = 0;
  for (const root of edges.keys()) {
    if (index.has(root)) continue;
    const work = [[root, [...(edges.get(root) || [])], 0]];
    index.set(root, next); low.set(root, next); next++; st.push(root); on.add(root);
    while (work.length) {
      const top = work[work.length - 1];
      const [v, succ] = top;
      if (top[2] < succ.length) {
        const w = succ[top[2]++];
        if (!index.has(w)) {
          index.set(w, next); low.set(w, next); next++; st.push(w); on.add(w);
          work.push([w, [...(edges.get(w) || [])], 0]);
        } else if (on.has(w)) low.set(v, Math.min(low.get(v), index.get(w)));
        continue;
      }
      work.pop();
      if (work.length) { const u = work[work.length - 1][0]; low.set(u, Math.min(low.get(u), low.get(v))); }
      if (low.get(v) !== index.get(v)) continue;
      const comp = [];
      for (;;) { const w = st.pop(); on.delete(w); comp.push(w); if (w === v) break; }
      if (comp.length > 1 || (edges.get(v) || new Set()).has(v)) out.push(comp.sort());
    }
  }
  return out;
}

/* The control: invented graphs whose cycles are known, so a census that cannot find a cycle cannot pass. */
function control() {
  const g = (pairs) => {
    const m = new Map();
    for (const [a, b] of pairs) { if (!m.has(a)) m.set(a, new Set()); if (b) m.get(a).add(b); }
    return m;
  };
  const cases = [
    [g([['a', 'b'], ['b', 'c']]), 0],
    [g([['a', 'a']]), 1],
    [g([['a', 'b'], ['b', 'a'], ['b', 'c']]), 1],
    [g([['a', 'b'], ['b', 'c'], ['c', 'a'], ['c', 'd'], ['d', 'd']]), 2],
  ];
  for (const [graph, want] of cases)
    if (cycles(graph).length !== want) die(`its own control failed: an invented graph with ${want} cycle(s) read ` +
                                           `${cycles(graph).length}, so a reading of the binary would mean nothing`);
}

/* A function's area, by what it links from: Lexbor by its symbol prefix (its objects carry relative paths),
   everything else by the source path its debug line names. */
function area(name, file) {
  if (/^(lxb_|lexbor_)/.test(name)) return 'lexbor';
  for (const [key, label] of [['/engine/qjs/', 'quickjs'], ['/engine/host/browser/', 'browser'],
                              ['/engine/host/solver/', 'solver'], ['/engine/host/', 'host']])
    if (file.includes(key)) return label;
  return 'other';
}

control();
if (!bin) die('usage: node engine/recgate.mjs <native binary> [--list]');
if (!existsSync(bin)) die(`no binary at ${bin}`);
/* The disassembly is streamed: a whole engine's runs past what one string may hold. */
const fnRe = /^([0-9a-f]+) <([^>]+)>:$/;
const callRe = /\s(call|jmp)\s+[0-9a-f]+ <([^>+]+)(\+0x[0-9a-f]+)?>/;
const addr = new Map(), edges = new Map();
let cur = null;
const od = spawn('objdump', ['-d', '--no-show-raw-insn', bin], { stdio: ['ignore', 'pipe', 'inherit'] });
const odExit = new Promise((resolve) => { od.on('error', (e) => die(`objdump could not run: ${e.message}`)); od.on('close', resolve); });
for await (const line of createInterface({ input: od.stdout, crlfDelay: Infinity })) {
  const f = fnRe.exec(line);
  if (f) { cur = f[2]; addr.set(cur, f[1]); if (!edges.has(cur)) edges.set(cur, new Set()); continue; }
  if (cur === null) continue;
  const c = callRe.exec(line);
  /* A call or tail jump to a function's first instruction; a jump back to one's own entry is a loop. */
  if (!c || c[3] || c[2].endsWith('@plt') || (c[1] === 'jmp' && c[2] === cur)) continue;
  edges.get(cur).add(c[2]);
}
const odCode = await odExit;
if (odCode !== 0) die(`objdump exited ${odCode} on ${bin}, so the graph is partial and nothing was judged`);
if (!addr.has('main')) die(`${bin} disassembled to no \`main\`, so it is not the engine binary this census reads`);

const found = cycles(edges);
const members = [...new Set(found.flat())].filter((n) => addr.has(n));
const lines = members.length
  ? execFileSync('addr2line', ['-e', bin, ...members.map((n) => addr.get(n))], { maxBuffer: 1 << 28, encoding: 'utf8' })
      .split('\n')
  : [];
const fileOf = new Map(members.map((n, i) => [n, (lines[i] || '?').split(':')[0]]));
const byArea = new Map();
found.sort((a, b) => b.length - a.length);
for (const comp of found) {
  const areas = [...new Set(comp.map((n) => area(n, fileOf.get(n) || '?')))].sort();
  for (const a of areas) byArea.set(a, (byArea.get(a) || 0) + 1);
  if (list || comp.length > 1)
    console.log(`[recgate] ${String(comp.length).padStart(4)}  ${areas.join(',').padEnd(16)} ` +
                comp.slice(0, 10).join(' ') + (comp.length > 10 ? ` … (+${comp.length - 10})` : ''));
}
/* The shortest cycle through `why`, by breadth-first search inside its own component. */
if (why) {
  const comp = found.find((c) => c.includes(why));
  if (!comp) console.log(`[recgate] --why=${why}: ${addr.has(why) ? 'it is in no cycle' : 'no such function'}`);
  else {
    const inComp = new Set(comp), prev = new Map([[why, null]]), queue = [why];
    let closer = null;
    while (queue.length && closer === null) {
      const v = queue.shift();
      for (const w of edges.get(v) || []) {
        if (w === why) { closer = v; break; }
        if (inComp.has(w) && !prev.has(w)) { prev.set(w, v); queue.push(w); }
      }
    }
    const chain = [why];
    for (let v = closer; v !== why; v = prev.get(v)) chain.splice(1, 0, v);
    console.log(`[recgate] --why=${why}: ${chain.join(' -> ')} -> ${why}`);
  }
}
const selfLoops = found.filter((c) => c.length === 1).length;
console.log(`[recgate] ${found.length} cycle(s) over ${addr.size} function(s) and ` +
            `${[...edges.values()].reduce((s, e) => s + e.size, 0)} direct call edge(s): ` +
            `${found.length - selfLoops} mutual, ${selfLoops} self (listed with --list); by area ` +
            [...byArea].map(([a, n]) => `${a} ${n}`).join(', '));
console.log('[recgate] not read: a call through a function pointer, so this count is a floor');
process.exit(found.length ? 1 : 0);
