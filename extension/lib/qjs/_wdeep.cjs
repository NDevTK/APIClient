// Verify the resumable/throttled deep stepping in the staged worker wasm:
// boot ONCE into qjsmain's persistent runtime, then --fe-deep-step batches
// reusing it (step 2+ must NOT re-boot → cheap), rem decreases to 0, the
// login-gated preheat endpoint is learned, no crash.
const fs = require('fs'), path = require('path');
const createQJS = require('./qjs_worker.js');
const Q = path.resolve(__dirname, '../../../engine/qjs');
const HOST = fs.readFileSync(Q + '/hostedge.js', 'utf8');
const DRV = fs.readFileSync(Q + '/driver.js', 'utf8');
const BUN = fs.readFileSync(Q + '/_livebundle.js', 'utf8');   // live crashing bundle dumped from Chrome feDeepDB
const PRE = fs.readFileSync(Q + '/_livebundle.pre.js', 'utf8');
const PJS = fs.readFileSync(Q + '/_curp.js', 'utf8');
(async () => {
  let out = [], err = [];
  const m = await createQJS({ noInitialRun: true, print: s => out.push(s), printErr: s => err.push(s), onAbort: () => err.push('ABORT') });
  m.FS.writeFile('/pre.js', PRE); m.FS.writeFile('/h.js', HOST); m.FS.writeFile('/p.js', PJS); m.FS.writeFile('/b.js', BUN); m.FS.writeFile('/d.js', DRV);
  // bytecode-cache the bundle
  try { m.callMain(['--fe-emit-bc', '/b.js', '/b.bc']); } catch (e) { err.push('emitbc ' + e); }
  const fileArgs = ['/pre.js', '/h.js', '/p.js', '/b.bc', '/d.js'];
  const BATCH = 3;
  let rem = 1, step = 0, preheat = false, totalFetch = 0, aborted = false, peakRss = 0, totalS = 0, totalZ = 0;
  const t0 = Date.now();
  let firstMs = 0, laterMax = 0, phStep = -1, drivenTotal = 0, phDriven = -1;
  var phUrl = "";
  while (rem > 0 && step < 100000) {   // run to completion: measure preheat's driven-position depth
    out.length = 0; err.length = 0;
    const st = Date.now();
    try { m.callMain(['--fe-deep-step=' + BATCH].concat(fileArgs)); } catch (e) { err.push('step ' + e); break; }
    const dt = Date.now() - st;
    if (step === 0) firstMs = dt; else laterMax = Math.max(laterMax, dt);
    rem = -1;
    for (const l of out) {
      if (l.slice(0, 4) === '@DS ') { try { rem = JSON.parse(l.slice(4)).rem; } catch (e) { rem = 0; } }
      if (l.slice(0, 4) === '@DD ') drivenTotal++;
      if (l.indexOf('preheat/index') >= 0) { if (!preheat) { phStep = step; phDriven = drivenTotal; } preheat = true; var _m = l.match(/"args":\["([^"]*)"/); if (_m) phUrl = _m[1]; }
      if (l.slice(0, 3) === '@H ' && l.indexOf('"fetch"') >= 0) totalFetch++;
      if (l.slice(0, 3) === '@S ') totalS++;
      if (l.slice(0, 3) === '@Z ') totalZ++;
    }
    const rss = process.memoryUsage().rss; if (rss > peakRss) peakRss = rss;
    if (err.some(e => /ABORT/.test(e))) { aborted = true; break; }
    if (rem < 0) rem = 0;
    step++;
    if (step <= 3 || step % 50 === 0) console.log(`step ${step}: ${dt}ms rem=${rem} preheat=${preheat} rss=${(rss/1048576|0)}MB`);
  }
  try { m.callMain(['--fe-deep-end']); } catch (e) {}
  console.log(`--- done: ${step} steps, ${((Date.now() - t0) / 1000).toFixed(1)}s, firstStep=${firstMs}ms laterMax=${laterMax}ms, totalFetch=${totalFetch}, @S=${totalS}, @Z=${totalZ}, preheat=${preheat}, aborted=${aborted}, peakRss=${(peakRss/1048576|0)}MB ---`);
  console.log(`preheat URL (named holes?): ${phUrl}`);
  console.log(`PREHEAT DEPTH: found at step=${phStep} after ${phDriven} orphans driven (of ${drivenTotal} total). At DEEP_ROUND=20 batches*3=60/cycle => ~${phDriven > 0 ? Math.ceil(phDriven / 60) : "?"} rotation cycles needed.`);
  console.log(`persistent-rt OK if laterMax << firstStep (no re-boot): firstStep=${firstMs}ms laterMax=${laterMax}ms`);
})();
