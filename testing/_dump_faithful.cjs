// Raw-CDP into the SW; reconstruct EXACTLY what _analyzeCombinedScripts
// hands the worker: combined /b.js + the /p.js page-DOM (data islands +
// byId + <script src>), using ast-thread's buildPageDomSrc verbatim.
// Writes /tmp/gh_b.js and /tmp/gh_p.js so drive.mjs can replay the
// faithful 4-file input offline.
const fs = require("fs");
const WebSocket = require("ws");

(async () => {
  const list = await (await fetch("http://127.0.0.1:9337/json/list")).json();
  const sw = list.find(t => t.type === "service_worker" && /background/.test(t.url || ""));
  if (!sw) { console.log("NO sw"); process.exit(1); }
  const ws = new WebSocket(sw.webSocketDebuggerUrl, { perMessageDeflate: false, maxPayload: 256 * 1024 * 1024 });
  let id = 0; const pend = new Map();
  ws.on("message", d => { const m = JSON.parse(String(d));
    if (m.id != null && pend.has(m.id)) { const p = pend.get(m.id); pend.delete(m.id);
      m.error ? p.rej(new Error(JSON.stringify(m.error))) : p.res(m.result); } });
  const send = (mth, p) => { const i = ++id; ws.send(JSON.stringify({ id: i, method: mth, params: p || {} }));
    return new Promise((res, rej) => pend.set(i, { res, rej })); };
  await new Promise((r, j) => { ws.once("open", r); ws.once("error", j); });
  await send("Runtime.enable");
  // buildPageDomSrc inlined verbatim from extension/ast-thread.js.
  const expr = `(function(){
    function buildPageDomSrc(domIslands, domContext, scriptUrls){
      var islands=Array.isArray(domIslands)?domIslands:[];
      var byId=(domContext&&domContext.byId&&typeof domContext.byId==='object')?domContext.byId:{};
      var srcs=Array.isArray(scriptUrls)?scriptUrls:[];
      if(!islands.length&&!Object.keys(byId).length&&!srcs.length)return null;
      return "(function(){try{\\n"+
        "var IS="+JSON.stringify(islands)+",BY="+JSON.stringify(byId)+",SR="+JSON.stringify(srcs)+";\\n"+
        "for(var i=0;i<IS.length;i++){var it=IS[i];var e=document.createElement(it.tag==='template'?'template':'script');"+
        "if(it.type)e.setAttribute('type',it.type);if(it.id)e.id=it.id;"+
        "if(it.dataTarget)e.setAttribute('data-target',it.dataTarget);e.textContent=it.text;"+
        "(document.head||document.documentElement||document.body).appendChild(e);}\\n"+
        "for(var id in BY){if(!Object.prototype.hasOwnProperty.call(BY,id)||document.getElementById(id))continue;"+
        "var b=BY[id],el=document.createElement('div');el.id=id;"+
        "if(b&&b.href)el.setAttribute('href',b.href);if(b&&b.src)el.setAttribute('src',b.src);"+
        "if(b&&b.action)el.setAttribute('action',b.action);"+
        "if(b&&b.dataAttrs)for(var k in b.dataAttrs)el.setAttribute('data-'+k,b.dataAttrs[k]);"+
        "(document.body||document.documentElement).appendChild(el);}\\n"+
        "var LS=null;for(var s=0;s<SR.length;s++){var sc=document.createElement('script');sc.src=SR[s];sc.setAttribute('src',SR[s]);(document.head||document.documentElement||document.body).appendChild(sc);LS=sc;}\\n"+
        "if(LS){try{Object.defineProperty(document,'currentScript',{get:function(){return LS;},configurable:true});}catch(e){}}\\n"+
        "}catch(e){}})();";
    }
    var tabId=null,b=null; for (const [t,x] of _scriptBuffers){tabId=t;b=x;}
    if(!b) return JSON.stringify({err:'no-buf'});
    b.scripts.sort(function(a,b){return (a.order==null?1e9:a.order)-(b.order==null?1e9:b.order);});
    var scripts=[],domIslands=[];
    for(var i=0;i<b.scripts.length;i++){var e=b.scripts[i];
      if(e.island)domIslands.push({id:e.island.id,type:e.island.scriptType,dataTarget:e.island.dataTarget,text:e.code});
      else scripts.push(e);}
    var scriptUrls=[];for(var j=0;j<scripts.length;j++)if(scripts[j].url)scriptUrls.push(scripts[j].url);
    var combined='';for(var k=0;k<scripts.length;k++){if(k>0)combined+=';\\n';combined+=scripts[k].code;}
    var dc=null; try{ dc=(getTab(tabId)||{}).domContext||null; }catch(e){}
    var pjs=buildPageDomSrc(domIslands,dc,scriptUrls);
    return JSON.stringify({bLen:combined.length, pLen:pjs?pjs.length:0, nScripts:scripts.length, nIslands:domIslands.length, nUrls:scriptUrls.length, _b:combined, _p:pjs||''});
  })()`;
  const r = await send("Runtime.evaluate", { expression: expr, returnByValue: true, timeout: 60000 });
  if (r.exceptionDetails) { console.log("EXC", JSON.stringify(r.exceptionDetails).slice(0, 500)); process.exit(1); }
  const o = JSON.parse(r.result.value);
  if (o.err) { console.log("ERR", o.err); process.exit(1); }
  fs.writeFileSync("/tmp/gh_b.js", o._b);
  fs.writeFileSync("/tmp/gh_p.js", o._p);
  console.log(`b=${o.bLen} p=${o.pLen} scripts=${o.nScripts} islands=${o.nIslands} urls=${o.nUrls}`);
  ws.close();
})().catch(e => { console.log("ERR", e.message); process.exit(1); });
