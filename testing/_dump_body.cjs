// Deterministic: raw-CDP the github PAGE target, pull document.body.innerHTML
// by value (no harness 30KB truncation, no SW in-memory confound).
const fs=require("fs"), WebSocket=require("ws");
(async()=>{
  const list=await(await fetch("http://127.0.0.1:9337/json/list")).json();
  const pg=list.find(t=>t.type==="page"&&/github\.com/.test(t.url||""));
  if(!pg){console.log("NO github page");process.exit(1);}
  const ws=new WebSocket(pg.webSocketDebuggerUrl,{perMessageDeflate:false,maxPayload:256*1024*1024});
  let id=0;const p=new Map();
  ws.on("message",d=>{const m=JSON.parse(String(d));if(m.id!=null&&p.has(m.id)){const x=p.get(m.id);p.delete(m.id);m.error?x.j(new Error(JSON.stringify(m.error))):x.r(m.result);}});
  const send=(me,pa)=>{const i=++id;ws.send(JSON.stringify({id:i,method:me,params:pa||{}}));return new Promise((r,j)=>p.set(i,{r,j}));};
  await new Promise((r,j)=>{ws.once("open",r);ws.once("error",j);});
  await send("Runtime.enable");
  const r=await send("Runtime.evaluate",{expression:"document.body.innerHTML",returnByValue:true,timeout:30000});
  const v=r.result&&r.result.value;
  if(typeof v!=="string"){console.log("BAD",JSON.stringify(r).slice(0,200));process.exit(1);}
  fs.writeFileSync("/tmp/gh_body.html",v);
  console.log("wrote /tmp/gh_body.html",v.length,"bytes; customEls:",(v.match(/<[a-z]+-[a-z-]+[ >]/g)||[]).length);
  ws.close();
})().catch(e=>{console.log("ERR",e.message);process.exit(1);});
