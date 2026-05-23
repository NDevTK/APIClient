const fs=require('fs'); const path=require('path');
const createQJS=require('./qjs_worker.js');
const Q=path.resolve(__dirname,'../../../engine/qjs');
const HOST=fs.readFileSync(Q+'/hostedge.js','utf8');
const DRV=fs.readFileSync(Q+'/driver.js','utf8');
const BUN=fs.readFileSync(Q+'/_curbundle.js','utf8');
const PRE=fs.readFileSync(Q+'/_realph.js','utf8');
const PJS=fs.readFileSync(Q+'/_curp.js','utf8');
(async()=>{
  let out=[],err=[];
  const m=await createQJS({noInitialRun:true,print:s=>out.push(s),printErr:s=>err.push(s),onAbort:()=>err.push('ABORT')});
  m.FS.writeFile('/pre.js',PRE); m.FS.writeFile('/h.js',HOST); m.FS.writeFile('/p.js',PJS); m.FS.writeFile('/b.js',BUN); m.FS.writeFile('/d.js',DRV);
  try{ m.callMain(['--fe-emit-bc','/b.js','/b.bc']); }catch(e){err.push('emitbc threw '+e);}
  out.length=0; err.length=0;
  const t0=Date.now();
  try{ m.callMain(['--fe-exec','--fe-sched=','--fe-trace=/t.tr','/pre.js','/h.js','/p.js','/b.bc','/d.js']); }catch(e){err.push('callMain threw '+e);}
  const ms=Date.now()-t0;
  const H=out.filter(s=>s.startsWith('@H'));
  const joined=out.join('\n');
  const chunks=new Set((joined.match(/assets\/(\d+)-/g)||[]).map(s=>s.replace(/assets\/|-/g,'')));
  console.log('time='+ms+'ms total @H='+H.length+' fetch='+H.filter(s=>s.includes('"fetch"')).length+' script='+H.filter(s=>s.includes('"script"')).length);
  console.log('distinct chunks='+chunks.size+'  30129 found='+joined.includes('assets/30129-'));
  console.log('@E count='+out.filter(s=>s.startsWith('@E')).length);
  console.log('err sample: '+err.slice(0,3).join(' | '));
})();
