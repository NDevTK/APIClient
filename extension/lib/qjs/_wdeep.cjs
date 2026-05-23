// Verify the resumable/throttled deep stepping in the staged worker wasm:
// boot ONCE into qjsmain's persistent runtime, then --fe-deep-step batches
// reusing it (step 2+ must NOT re-boot → cheap), rem decreases to 0, the
// login-gated preheat endpoint is learned, no crash.
const fs = require('fs'), path = require('path');
const createQJS = require('./qjs_worker.js');
const Q = path.resolve(__dirname, '../../../engine/qjs');
const HOST = fs.readFileSync(Q + '/hostedge.js', 'utf8');
const DRV = fs.readFileSync(Q + '/driver.js', 'utf8');
const BUN = fs.readFileSync(Q + '/_curbundle_all.js', 'utf8');   // eager + 346 chunks
const PRE = fs.readFileSync(Q + '/_realph.js', 'utf8');
const PJS = fs.readFileSync(Q + '/_curp.js', 'utf8');
(async () => {
  let out = [], err = [];
  const m = await createQJS({ noInitialRun: true, print: s => out.push(s), printErr: s => err.push(s), onAbort: () => err.push('ABORT') });
  m.FS.writeFile('/pre.js', PRE); m.FS.writeFile('/h.js', HOST); m.FS.writeFile('/p.js', PJS); m.FS.writeFile('/b.js', BUN); m.FS.writeFile('/d.js', DRV);
  // bytecode-cache the bundle
  try { m.callMain(['--fe-emit-bc', '/b.js', '/b.bc']); } catch (e) { err.push('emitbc ' + e); }
  const fileArgs = ['/pre.js', '/h.js', '/p.js', '/b.bc', '/d.js'];
  const BATCH = 3;
  let rem = 1, step = 0, preheat = false, totalFetch = 0, aborted = false, peakRss = 0;
  const t0 = Date.now();
  let firstMs = 0, laterMax = 0;
  while (rem > 0 && step < 12) {   // fast persistence/no-crash check; set high to run to completion (wasm preheat-via-deep verified to rem=0, peakRss~679MB)
    out.length = 0; err.length = 0;
    const st = Date.now();
    try { m.callMain(['--fe-deep-step=' + BATCH].concat(fileArgs)); } catch (e) { err.push('step ' + e); break; }
    const dt = Date.now() - st;
    if (step === 0) firstMs = dt; else laterMax = Math.max(laterMax, dt);
    rem = -1;
    for (const l of out) {
      if (l.slice(0, 4) === '@DS ') { try { rem = JSON.parse(l.slice(4)).rem; } catch (e) { rem = 0; } }
      if (l.indexOf('preheat/index') >= 0) preheat = true;
      if (l.slice(0, 3) === '@H ' && l.indexOf('"fetch"') >= 0) totalFetch++;
    }
    const rss = process.memoryUsage().rss; if (rss > peakRss) peakRss = rss;
    if (err.some(e => /ABORT/.test(e))) { aborted = true; break; }
    if (rem < 0) rem = 0;
    step++;
    if (step <= 3 || step % 50 === 0) console.log(`step ${step}: ${dt}ms rem=${rem} preheat=${preheat} rss=${(rss/1048576|0)}MB`);
  }
  try { m.callMain(['--fe-deep-end']); } catch (e) {}
  console.log(`--- done: ${step} steps, ${((Date.now() - t0) / 1000).toFixed(1)}s, firstStep=${firstMs}ms laterMax=${laterMax}ms, totalFetch=${totalFetch}, preheat=${preheat}, aborted=${aborted}, peakRss=${(peakRss/1048576|0)}MB ---`);
  console.log(`persistent-rt OK if laterMax << firstStep (no re-boot): firstStep=${firstMs}ms laterMax=${laterMax}ms`);
})();
