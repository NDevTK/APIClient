/* Self-hosted JS prelude — the JavaScript the engine evaluates at init, like V8's builtins/*.js.
 * ARRAY_PRELUDE_JS: self-hosted Array/String iterators (forEach/map/reduce/...) as BYTECODE so recursion
 * through a callback is a heap frame (trampolined, unbounded) instead of a C-stack overflow. DEDUP_JS: the
 * in-engine endpoint identity/dedup run at finalize over the accumulated @H records (the cumulative moat
 * aggregation the engine owns). Both compiled under JS_SetBuiltinCompile (born executed=1, never orphan-
 * driven). Kept as their own TU so main.c is the SCHEDULER, not a wall of embedded JS. See prelude.h. */
#include "prelude.h"

const char *ARRAY_PRELUDE_JS =
/* An OPAQUE array (a reply/injected-state/attacker value, unknown length) FORKS UNBOUNDED like any opaque
   loop: __alen returns O.length UNCHANGED (opaque survives >>>0), so `k < __alen(O)` is an opaque compare
   that forks (continue vs exit) each iteration. NOT 'driven once' — predicting iterations 2..N redundant is
   the banned seen-set (§EVERY-RUNTIME-JOB / opacity-boundary). Safe now that each cb call is its OWN bounded
   flow (the cb-hook) and the loop index is frame-local (not COW), so the deep continue-tail is parkable +
   value-outranked (paged to disk), never a single-delta OOM. __ain: an opaque array always "has" index k so
   cb still runs. Concrete arrays are unchanged. */
"var __alen=function(O){return O.length>>>0;};"
"var __ain=function(O,k){return __isOpaque(O)||(k in O);};"
"Object.defineProperty(Array.prototype,'forEach',{writable:true,enumerable:false,configurable:true,value:function forEach(cb,thisArg){"
"  if(typeof cb!=='function')throw new TypeError('Array.prototype.forEach: callback is not a function');"
"  var O=Object(this),L=__alen(O);"
"  for(var k=0;k<L;k++){if(__ain(O,k)){cb.call(thisArg,O[k],k,O);}}return undefined;}});"
"Object.defineProperty(Array.prototype,'map',{writable:true,enumerable:false,configurable:true,value:function map(cb,thisArg){"  /* holes preserved; species not honored (plain Array) */
"  if(typeof cb!=='function')throw new TypeError('Array.prototype.map: callback is not a function');"
"  var O=Object(this),L=__alen(O),A=new Array(L);"
"  for(var k=0;k<L;k++){if(__ain(O,k)){A[k]=cb.call(thisArg,O[k],k,O);}}return A;}});"
"Object.defineProperty(Array.prototype,'filter',{writable:true,enumerable:false,configurable:true,value:function filter(cb,thisArg){"
"  if(typeof cb!=='function')throw new TypeError('Array.prototype.filter: callback is not a function');"
"  var O=Object(this),L=__alen(O),A=[],n=0;"
"  for(var k=0;k<L;k++){if(__ain(O,k)){var v=O[k];if(cb.call(thisArg,v,k,O))A[n++]=v;}}return A;}});"
"Object.defineProperty(Array.prototype,'some',{writable:true,enumerable:false,configurable:true,value:function some(cb,thisArg){"  /* early-exit on first truthy */
"  if(typeof cb!=='function')throw new TypeError('Array.prototype.some: callback is not a function');"
"  var O=Object(this),L=__alen(O);"
"  for(var k=0;k<L;k++){if(__ain(O,k)){if(cb.call(thisArg,O[k],k,O))return true;}}return false;}});"
"Object.defineProperty(Array.prototype,'every',{writable:true,enumerable:false,configurable:true,value:function every(cb,thisArg){"  /* early-exit on first falsy */
"  if(typeof cb!=='function')throw new TypeError('Array.prototype.every: callback is not a function');"
"  var O=Object(this),L=__alen(O);"
"  for(var k=0;k<L;k++){if(__ain(O,k)){if(!cb.call(thisArg,O[k],k,O))return false;}}return true;}});"
/* includes/indexOf are SELF-HOSTED not for overflow but for OPACITY-BY-CONSTRUCTION: the C builtins collapse
   `arr.includes(opaqueInput)` to a concrete false/-1 (identity compare), so a list-membership GATE
   (`if(allowed.includes(input))`) never forks and the gated code is never explored (missed @H). As bytecode
   the compare is OP_strict_eq, which propagates opacity -> the branch forks (explore) AND records the
   `{src}==element` constraint, so an ORIGIN whitelist auto-suppresses @S via cons_fixed_value while a DATA
   whitelist stays a real reachable sink. One mechanism, both outcomes. (SameValueZero for includes; strict
   === + hole-skip for indexOf; fromIndex via |0 — the prelude's small-index simplification.) */
"Object.defineProperty(Array.prototype,'includes',{writable:true,enumerable:false,configurable:true,value:function includes(sv,fi){"
"  var O=Object(this),L=__alen(O); if(L===0)return false;"
"  var n=fi|0,k=n>=0?n:L+n; if(k<0)k=0;"
"  for(;k<L;k++){var v=O[k]; if(v===sv||(v!==v&&sv!==sv))return true;} return false;}});"
"Object.defineProperty(Array.prototype,'indexOf',{writable:true,enumerable:false,configurable:true,value:function indexOf(sv,fi){"
"  var O=Object(this),L=__alen(O); if(L===0)return -1;"
"  var n=fi|0,k=n>=0?n:L+n; if(k<0)k=0;"
"  for(;k<L;k++){if(__ain(O,k)&&O[k]===sv)return k;} return -1;}});"
/* lastIndexOf (=== + hole-skip, reverse) and find/findIndex (PREDICATE) collapse the same way: the C
   builtins run JS_ToBool on an opaque predicate-result -> concrete -> no fork. Self-hosted, the truthiness
   test is OP_if which propagates opacity, so `arr.find(x=>x===input)`-gated code forks + explores. */
"Object.defineProperty(Array.prototype,'lastIndexOf',{writable:true,enumerable:false,configurable:true,value:function lastIndexOf(sv,fi){"
"  var O=Object(this),L=__alen(O); if(L===0)return -1;"
"  var n=arguments.length>1?fi|0:L-1,k=n>=0?(n<L-1?n:L-1):L+n;"
"  for(;k>=0;k--){if(__ain(O,k)&&O[k]===sv)return k;} return -1;}});"
"Object.defineProperty(Array.prototype,'find',{writable:true,enumerable:false,configurable:true,value:function find(cb,thisArg){"
"  if(typeof cb!=='function')throw new TypeError('Array.prototype.find: callback is not a function');"
"  var O=Object(this),L=__alen(O);"
"  for(var k=0;k<L;k++){var v=O[k]; if(cb.call(thisArg,v,k,O))return v;} return undefined;}});"
"Object.defineProperty(Array.prototype,'findIndex',{writable:true,enumerable:false,configurable:true,value:function findIndex(cb,thisArg){"
"  if(typeof cb!=='function')throw new TypeError('Array.prototype.findIndex: callback is not a function');"
"  var O=Object(this),L=__alen(O);"
"  for(var k=0;k<L;k++){if(cb.call(thisArg,O[k],k,O))return k;} return -1;}});"
/* reduce/reduceRight: siblings of forEach/map, self-hosted for the SAME two reasons — (1) OVERFLOW: the C
   builtin holds the accumulator on the C stack across each callback, so `arr.reduce((_,v)=>recur(v))` C-recurses
   and TRAPS the wasm stack (hard abort, not a catchable throw); as bytecode the callback dispatches via the
   trampolined OP_call, unbounded. (2) OPACITY: the callback runs as bytecode so an opaque accumulator/element
   propagates through it. No thisArg (spec: callback `this` is undefined) -> plain cb(...). */
"Object.defineProperty(Array.prototype,'reduce',{writable:true,enumerable:false,configurable:true,value:function reduce(cb,iv){"
"  if(typeof cb!=='function')throw new TypeError('Array.prototype.reduce: callback is not a function');"
"  var O=Object(this),L=__alen(O),k=0,acc;"
"  if(arguments.length>1){acc=iv;}else{while(k<L&&!(__ain(O,k)))k++;if(k>=L)throw new TypeError('Reduce of empty array with no initial value');acc=O[k++];}"
"  for(;k<L;k++){if(__ain(O,k))acc=cb(acc,O[k],k,O);}return acc;}});"
"Object.defineProperty(Array.prototype,'reduceRight',{writable:true,enumerable:false,configurable:true,value:function reduceRight(cb,iv){"
"  if(typeof cb!=='function')throw new TypeError('Array.prototype.reduceRight: callback is not a function');"
"  var O=Object(this),L=__alen(O),k=L-1,acc;"
"  if(arguments.length>1){acc=iv;}else{while(k>=0&&!(__ain(O,k)))k--;if(k<0)throw new TypeError('Reduce of empty array with no initial value');acc=O[k--];}"
"  for(;k>=0;k--){if(__ain(O,k))acc=cb(acc,O[k],k,O);}return acc;}});"
/* Array.sort: SELF-HOSTED (iterative bottom-up merge, stable) so the COMPARATOR dispatches via the trampolined
   OP_call — deep comparator recursion is UNBOUNDED, not a C-stack trap. The ordering branch must NOT fork on an
   OPAQUE compare (element order is meaningless for @H/@S; O(n log n) forks would explode): `__isOpaque` (a
   concrete-bool leaf) collapses a meaningless opaque order to 0, while the comparator STILL runs (its emits/side
   effects happen). Concrete comparisons use plain JS operators (spec-correct UTF-16 default order; a concrete
   compare never forks). No nested function (that crashes the trampoline). undefined sorts after all values, holes
   last (comparator never sees them); NaN comparator result -> 0; comparator `this` is undefined; in-place. */
"Object.defineProperty(Array.prototype,'sort',{writable:true,enumerable:false,configurable:true,value:function sort(cmp){"
"  if(cmp!==undefined&&typeof cmp!=='function')throw new TypeError('Array.prototype.sort: comparator is not a function');"
"  var O=Object(this),L=__alen(O),items=[],undef=0,holes=0,k;"
"  for(k=0;k<L;k++){if(__ain(O,k)){var v=O[k];if(v===undefined)undef++;else items.push(v);}else holes++;}"
"  var cf=cmp?function(a,b){var r=cmp(a,b);if(__isOpaque(r))return 0;var d=+r;return d!==d?0:d;}"
"           :function(a,b){if(__isOpaque(a)||__isOpaque(b))return 0;var sa=''+a,sb=''+b;return sa<sb?-1:(sa>sb?1:0);};"
"  var N=items.length,buf=new Array(N),w,lo,mid,hi,i,j,t;"
"  for(w=1;w<N;w*=2){for(lo=0;lo<N;lo+=2*w){mid=lo+w<N?lo+w:N;hi=lo+2*w<N?lo+2*w:N;i=lo;j=mid;t=lo;"
"    while(i<mid&&j<hi){if(cf(items[i],items[j])<=0)buf[t++]=items[i++];else buf[t++]=items[j++];}"
"    while(i<mid)buf[t++]=items[i++];while(j<hi)buf[t++]=items[j++];"
"    for(t=lo;t<hi;t++)items[t]=buf[t];}}"
"  var idx=0;"
"  for(k=0;k<items.length;k++)O[idx++]=items[k];"
"  for(k=0;k<undef;k++)O[idx++]=undefined;"
"  for(k=0;k<holes;k++)delete O[idx++];"
"  return O;}});"
/* String.prototype.replace with a FUNCTION replacer: self-hosted so recursion THROUGH the replacer
   trampolines (unbounded, like reduce) and an opaque match/group propagates through it. Only the function
   path is self-hosted; a STRING replacer delegates to the original C builtin (`_o`, captured in the IIFE so
   no global is polluted) which keeps all the $&/$1/$<name> substitution semantics. Flat while-loop, no nested
   function (that crashes the trampoline). Matches native across 40k randomized regex/string/function cases.
   `repl.apply` is a tail-forward (already trampolined); exec is a leaf C call holding no continuation. */
"(function(){var _o=String.prototype.replace;"
"Object.defineProperty(String.prototype,'replace',{writable:true,enumerable:false,configurable:true,value:function replace(search,repl){"
"  if(typeof repl!=='function')return _o.call(this,search,repl);"
"  var str=String(this);"
"  if(search instanceof RegExp){var g=search.global,out='',last=0,m;if(g)search.lastIndex=0;"
"    while((m=search.exec(str))!==null){var ms=m[0],off=m.index,args=Array.prototype.slice.call(m);args.push(off,str);if(m.groups!==undefined)args.push(m.groups);"
"      out+=str.slice(last,off)+String(repl.apply(undefined,args));last=off+ms.length;if(!g)break;if(ms.length===0)search.lastIndex++;}"
"    return out+str.slice(last);}"
"  var ss=String(search),i=str.indexOf(ss);if(i===-1)return str;"
"  return str.slice(0,i)+String(repl(ss,i,str))+str.slice(i+ss.length);}});})();"
/* JSON.stringify: self-hosted so deep object-graph recursion AND a recursive replacer trampoline (unbounded).
   Q/BS/NL = fromCharCode(34/92/10) keep literal quote/backslash/newline OUT of this C string. __isOpaque
   short-circuits an OPAQUE value to ITSELF (taint PRESERVED — never de-tainted to a placeholder) BEFORE any
   typeof/type-dispatch that would fork on it. Spec-faithful (40k-case node differential vs native): toJSON,
   replacer fn/array, space (number clamped<=10 / string), wrapper unwrap, circular throws, undefined/function
   omitted, valid surrogate pairs literal + lone surrogates escaped. Recursive `ser` (nested-closure non-tail
   recursion — needs the trampoline caller_var_refs fix). */
"(function(){var Q=String.fromCharCode(34),BS=String.fromCharCode(92),NL=String.fromCharCode(10);"
"JSON.stringify=function stringify(value,replacer,space){"
"  var replacerFn=null,propertyList=null;"
"  if(typeof replacer==='function')replacerFn=replacer;"
"  else if(Array.isArray(replacer)){propertyList=[];var seen={};"
"    for(var ri=0;ri<replacer.length;ri++){var rv=replacer[ri],item;"
"      if(typeof rv==='string')item=rv;else if(typeof rv==='number')item=String(rv);"
"      else if(rv&&(rv instanceof String||rv instanceof Number))item=String(rv);else continue;"
"      if(!seen[item]){seen[item]=1;propertyList.push(item);}}}"
"  var gap='';"
"  if(typeof space==='object'&&space!==null){if(space instanceof Number)space=Number(space);else if(space instanceof String)space=String(space);}"
"  if(typeof space==='number'){var sn=space<10?Math.floor(space):10;if(sn>=1)gap=' '.repeat(sn);}"
"  else if(typeof space==='string')gap=space.length<=10?space:space.slice(0,10);"
"  var stack=[],indent='';"
"  function quote(s){var out=Q;for(var i=0;i<s.length;i++){var c=s.charCodeAt(i),ch=s.charAt(i);"
"    if(ch===Q)out+=BS+Q;else if(ch===BS)out+=BS+BS;"
"    else if(c===8)out+=BS+'b';else if(c===12)out+=BS+'f';else if(c===10)out+=BS+'n';else if(c===13)out+=BS+'r';else if(c===9)out+=BS+'t';"
"    else if(c<32){var h=c.toString(16);out+=BS+'u'+'0000'.slice(h.length)+h;}"
"    else if(c>=55296&&c<=57343){var c2=i+1<s.length?s.charCodeAt(i+1):0;"
"      if(c<=56319&&c2>=56320&&c2<=57343){out+=ch+s.charAt(i+1);i++;}else{var h2=c.toString(16);out+=BS+'u'+'0000'.slice(h2.length)+h2;}}"
"    else out+=ch;}return out+Q;}"
"  function ser(key,holder){var value=holder[key];"
"    if(__isOpaque(value)){var _ex=__opaqueExample(value);if(_ex===undefined)return value;value=_ex;}"
"    if(value!==null&&typeof value==='object'&&typeof value.toJSON==='function')value=value.toJSON(key);"
"    if(replacerFn)value=replacerFn.call(holder,key,value);"
"    if(__isOpaque(value)){var _ex=__opaqueExample(value);if(_ex===undefined)return value;value=_ex;}"
"    if(value!==null&&typeof value==='object'){if(value instanceof Number)value=Number(value);else if(value instanceof String)value=String(value);else if(value instanceof Boolean)value=value.valueOf();}"
"    if(value===null)return 'null';if(value===true)return 'true';if(value===false)return 'false';"
"    var t=typeof value;"
"    if(t==='string')return quote(value);"
"    if(t==='number')return isFinite(value)?String(value):'null';"
"    if(t==='bigint')throw new TypeError('Do not know how to serialize a BigInt');"
"    if(t==='object'){"
"      for(var si=0;si<stack.length;si++)if(stack[si]===value)throw new TypeError('Converting circular structure to JSON');"
"      stack.push(value);var stepback=indent;indent+=gap;var res;"
"      if(Array.isArray(value)){var parts=[];for(var ai=0;ai<value.length;ai++){var e=ser(String(ai),value);"
"        if(__isOpaque(e))parts.push(e);else parts.push(e===undefined?'null':e);}"
"        if(parts.length===0)res='[]';else if(gap==='')res='['+parts.join(',')+']';else res='['+NL+indent+parts.join(','+NL+indent)+NL+stepback+']';}"
"      else{var keys=propertyList||Object.keys(value),members=[];"
"        for(var ki=0;ki<keys.length;ki++){var pk=keys[ki],ps=ser(pk,value);"
"          if(__isOpaque(ps)||ps!==undefined)members.push(quote(pk)+(gap===''?':':': ')+ps);}"
"        if(members.length===0)res='{}';else if(gap==='')res='{'+members.join(',')+'}';else res='{'+NL+indent+members.join(','+NL+indent)+NL+stepback+'}';}"
"      stack.pop();indent=stepback;return res;}"
"    return undefined;}"
"  var wrapper={};wrapper['']=value;return ser('',wrapper);};})();";

const char *DEDUP_JS =
"(function(eps){"
"  var HOLE=/\\{[a-z]*\\}/,HOLE_G=/\\{[a-z]*\\}/g,SEG_HOLE=/^\\{[a-z]*\\}$/;"
"  var hasHole=function(s){return HOLE.test(s||'');};"
"  var normHoles=function(s){return (s||'').replace(HOLE_G,'{}');};"
"  var pathSegs=function(u){var q=u.indexOf('?');var p=q>=0?u.slice(0,q):u;return {segs:p.split('/'),query:q>=0?u.slice(q):''};};"
"  for(var bi=0;bi<eps.length;bi++){var e=eps[bi];"
"    if(e.body){var bo=null;try{bo=JSON.parse(e.body);}catch(_e){bo=null;}"
"      if(bo&&typeof bo==='object'&&!Array.isArray(bo)){e.params=e.params||[];"
"        for(var bk in bo){var bv=bo[bk];"
"          var op=bv===null||bv==='{}'||(typeof bv==='object'&&bv&&!Array.isArray(bv)&&Object.keys(bv).length===0);"
"          var has=false;for(var qi=0;qi<e.params.length;qi++){if(e.params[qi].name===bk&&e.params[qi].location==='body'){has=true;break;}}"
"          if(!has)e.params.push({name:bk,location:'body',validValues:op?[]:[(typeof bv==='object'&&bv)?JSON.stringify(bv):String(bv)]});"
"        }"
"      }else if(e.body.indexOf('=')>=0&&e.body.charAt(0)!=='{'&&e.body.charAt(0)!=='['){e.params=e.params||[];"
"        var frms=e.body.split('&');for(var fpi=0;fpi<frms.length;fpi++){var feq=frms[fpi].indexOf('=');if(feq<0)continue;"
"          var fk=frms[fpi].slice(0,feq),fv=frms[fpi].slice(feq+1),fop=hasHole(fv);"
"          var fhas=false;for(var fqi=0;fqi<e.params.length;fqi++){if(e.params[fqi].name===fk&&e.params[fqi].location==='body'){fhas=true;break;}}"
"          if(!fhas)e.params.push({name:fk,location:'body',validValues:fop?[]:[fv]});"
"        }"
"      }"
"    }"
"  }"
"  var map=new Map();"
"  var mergeInto=function(e,s){"                                                 /* UNION s into e (same identity) */
"    var sp=s.params||[];e.params=e.params||[];"
"    for(var pi=0;pi<sp.length;pi++){var np=sp[pi],f=null;"
"      for(var ei=0;ei<e.params.length;ei++){if(e.params[ei].name===np.name&&e.params[ei].location===np.location){f=e.params[ei];break;}}"
"      if(!f){e.params.push(np);}else{var nv=np.validValues||[];f.validValues=f.validValues||[];for(var vi=0;vi<nv.length;vi++){if(f.validValues.indexOf(nv[vi])<0)f.validValues.push(nv[vi]);}}"
"    }"
"    if(s.headers){e.headers=e.headers||{};var HH=/\\{[a-z]*\\}/;for(var hk in s.headers){var ov=e.headers[hk],nv=s.headers[hk];if(ov===undefined||(HH.test(ov)&&!HH.test(nv)))e.headers[hk]=nv;}}"
"    if(s.body&&(!e.body||(/\\{[a-z]*\\}/.test(e.body)&&!/\\{[a-z]*\\}/.test(s.body))))e.body=s.body;"
"  };"
"  for(var i=0;i<eps.length;i++){var s=eps[i];var k=(s.method||'GET')+' '+normHoles(s.url);if(!map.has(k))map.set(k,s);else mergeInto(map.get(k),s);}"
"  var arr=[];map.forEach(function(v){arr.push(v);});"
"  var shapes=arr.filter(function(e){return hasHole(e.url);});"
"  if(shapes.length){"
"    for(var ci=0;ci<arr.length;ci++){var c=arr[ci];if(hasHole(c.url))continue;"
"      for(var si=0;si<shapes.length;si++){var sh=shapes[si];"
"        if((sh.method||'GET')!==(c.method||'GET'))continue;"
"        var ss=pathSegs(sh.url),cs=pathSegs(c.url);"
"        if(ss.segs.length!==cs.segs.length||ss.query!==cs.query)continue;"
"        var ok=true,ex=[];"
"        for(var j=0;j<ss.segs.length;j++){"
"          if(SEG_HOLE.test(ss.segs[j])){if(cs.segs[j]&&!hasHole(cs.segs[j]))ex.push([j,cs.segs[j]]);else{ok=false;break;}}"
"          else if(ss.segs[j]!==cs.segs[j]){ok=false;break;}"
"        }"
"        if(ok&&ex.length){"
"          for(var e2=0;e2<ex.length;e2++){var idx=ex[e2][0],v=ex[e2][1];"
"            var pp=null,pl=sh.params||[];for(var pi=0;pi<pl.length;pi++){if(pl[pi].location==='path'&&pl[pi].name==='arg'+idx){pp=pl[pi];break;}}"
"            if(!pp){pp={name:'arg'+idx,location:'path',validValues:[]};(sh.params=sh.params||[]).push(pp);}"
"            if(pp.validValues.indexOf(v)<0)pp.validValues.push(v);"
"          }"
"          map.delete((c.method||'GET')+' '+c.url);break;"
"        }"
"      }"
"    }"
"  }"
"  var parseQ=function(q){var o={};if(!q)return o;var s=q.charAt(0)==='?'?q.slice(1):q;if(!s)return o;var ps=s.split('&');for(var i=0;i<ps.length;i++){var eq=ps[i].indexOf('=');o[eq>=0?ps[i].slice(0,eq):ps[i]]=eq>=0?ps[i].slice(eq+1):'';}return o;};"
"  arr=[];map.forEach(function(v){arr.push(v);});"                                        /* QUERY-PHANTOM collapse: a bare {} query value (from an opaque-degraded run) is a PHANTOM of its concrete sibling (the boot re-run's concolic example) — delete it so the real value wins, not the {} */
"  for(var qi=0;qi<arr.length;qi++){var ph0=arr[qi];var ps0=pathSegs(ph0.url);if(ps0.query.indexOf('{}')<0)continue;var pq=parseQ(ps0.query),pk=Object.keys(pq);"
"    for(var cj=0;cj<arr.length;cj++){var cc=arr[cj];if(cc===ph0)continue;if((cc.method||'GET')!==(ph0.method||'GET'))continue;var cs0=pathSegs(cc.url);"
"      if(ps0.segs.join('/')!==cs0.segs.join('/'))continue;var cq0=parseQ(cs0.query);if(Object.keys(cq0).length!==pk.length)continue;"
"      var ok0=true,better=false;for(var ki=0;ki<pk.length;ki++){var kk=pk[ki];if(!(kk in cq0)){ok0=false;break;}"
"        if(pq[kk]==='{}'){if(cq0[kk]!=='{}'&&!hasHole(cq0[kk]))better=true;}else if(pq[kk]!==cq0[kk]){ok0=false;break;}}"
"      if(ok0&&better){map.delete((ph0.method||'GET')+' '+ph0.url);break;}"                /* concrete sibling dominates -> drop the {} phantom */
"    }"
"  }"
"  var out=[];map.forEach(function(v){out.push(v);});return out;"
"})";
