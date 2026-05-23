const fs=require('fs'); const path=require('path');
const createQJS=require('./qjs_worker.js');
const Q=path.resolve(__dirname,'../../../engine/qjs');
(async()=>{
  let out=[],err=[];
  const m=await createQJS({noInitialRun:true,print:s=>out.push(s),printErr:s=>err.push(s),onAbort:()=>err.push('ABORT')});
  m.FS.writeFile('/pre.js',fs.readFileSync(Q+'/_realph.js','utf8'));
  m.FS.writeFile('/h.js',fs.readFileSync(Q+'/hostedge.js','utf8'));
  m.FS.writeFile('/p.js',fs.readFileSync(Q+'/_curp.js','utf8'));
  m.FS.writeFile('/probe.js','var g=(typeof globalThis==="object")?globalThis:this; throw new Error("WG|| doc="+(typeof g.document)+" imp="+(typeof g.importScripts)+" loc="+(g.location&&(g.location+""))+" cs="+(g.document&&g.document.currentScript&&g.document.currentScript.src)+" nScr="+(g.document&&g.document.getElementsByTagName("script").length));');
  try{ m.callMain(['--fe-exec','--fe-sched=','--fe-trace=/t.tr','/pre.js','/h.js','/p.js','/probe.js']); }catch(e){}
  console.log(out.concat(err).join('\n').match(/WG\|\|[^"\]*/)[0]);
})();
