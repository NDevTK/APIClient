const fs = require('fs'), path = require('path');
const createQJS = require('./qjs_worker.js');
const Q = path.resolve(__dirname, '../../../engine/qjs');
const HOST = fs.readFileSync(Q + '/hostedge.js', 'utf8');
const DRV = fs.readFileSync(Q + '/driver.js', 'utf8');
const BUN = fs.readFileSync(Q + '/_livebundle.js', 'utf8');
const PRE = fs.readFileSync(Q + '/_livebundle.pre.js', 'utf8');
const PJS = fs.readFileSync(Q + '/_curp.js', 'utf8');
(async () => {
  let out = [], err = [];
  const m = await createQJS({ noInitialRun: true, print: s => out.push(s), printErr: s => err.push(s), onAbort: () => err.push('ABORT') });
  m.FS.writeFile('/pre.js', PRE); m.FS.writeFile('/h.js', HOST); m.FS.writeFile('/p.js', PJS); m.FS.writeFile('/b.js', BUN); m.FS.writeFile('/d.js', DRV);
  try { m.callMain(['--fe-emit-bc', '/b.js', '/b.bc']); } catch (e) { console.log('EMITBC THREW:', e); }
  console.log('emitbc out tail:', out.slice(-5).join('\n'));
  console.log('emitbc err tail:', err.slice(-10).join('\n'));
  out.length=0; err.length=0;
  const fileArgs = ['/pre.js', '/h.js', '/p.js', '/b.bc', '/d.js'];
  try { m.callMain(['--fe-deep-step=3'].concat(fileArgs)); } catch (e) { console.log('STEP THREW:', e && e.message || e); }
  console.log('step out lines:', out.length, 'err lines:', err.length);
  console.log('step out tail:\n' + out.slice(-15).join('\n'));
  console.log('step err tail:\n' + err.slice(-20).join('\n'));
})();
