import binaryenImport from "binaryen";
const b = binaryenImport.default || binaryenImport;
import { readFileSync } from "fs";
function childRefs(info){switch(info.id){
 case b.BlockId:return info.children;case b.IfId:return [info.condition,info.ifTrue,info.ifFalse].filter(Boolean);
 case b.LoopId:return [info.body];case b.BreakId:return [info.value,info.condition].filter(Boolean);
 case b.SwitchId:return [info.condition,info.value].filter(Boolean);case b.CallId:return info.operands;
 case b.CallIndirectId:return [info.target,...info.operands];case b.LocalSetId:return [info.value];
 case b.GlobalSetId:return [info.value];case b.LoadId:return [info.ptr];case b.StoreId:return [info.ptr,info.value];
 case b.UnaryId:return [info.value];case b.BinaryId:return [info.left,info.right];
 case b.SelectId:return [info.ifTrue,info.ifFalse,info.condition];case b.DropId:return [info.value];
 case b.ReturnId:return info.value?[info.value]:[];case b.MemoryGrowId:return [info.delta];
 case b.MemoryFillId:return [info.dest,info.value,info.size];case b.MemoryCopyId:return [info.dest,info.source,info.size];
 case b.MemoryInitId:return [info.dest,info.offset,info.size];case b.TryId:return [info.body,...(info.catchBodies||[])];
 case b.ThrowId:return info.operands;default:return [];}}
const LEAF=new Set([b.NopId,b.UnreachableId,b.PopId,b.MemorySizeId,b.ConstId,b.LocalGetId,b.GlobalGetId,b.RefNullId,b.RefFuncId,b.DataDropId,b.ElemDropId,b.RethrowId].filter(x=>x!==undefined));
const m=b.readBinary(new Uint8Array(readFileSync("engine/qjs/qjs_worker.wasm")));
m.setFeatures(m.getFeatures()|b.Features.All);
let total=0,nonzero=0,maxoff=0; const hist={};
function walk(e){if(!e)return;const id=b.getExpressionId(e);
 if(id===b.StoreId){const info=b.getExpressionInfo(e);total++;const off=info.offset;
  if(off>0){nonzero++;if(off>maxoff)maxoff=off;}
  const k=off===0?"0":off<64?"1-63":off<256?"64-255":off<4096?"256-4K":off<65536?"4K-64K":">=64K";
  hist[k]=(hist[k]||0)+1;}
 if(LEAF.has(id))return;const info=b.getExpressionInfo(e);for(const c of childRefs(info))walk(c);}
for(let i=0;i<m.getNumFunctions();i++){const fi=b.getFunctionInfo(m.getFunctionByIndex(i));if(fi.body)walk(fi.body);}
console.log("total stores:",total,"| non-zero offset:",nonzero,`(${(100*nonzero/total).toFixed(1)}%)`,"| max offset:",maxoff);
console.log("offset histogram:",JSON.stringify(hist));
