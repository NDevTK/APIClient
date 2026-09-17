const i=r=>r.replace(/\.?0+$/,""),n=(r,e)=>{if(!r.toString().includes(".")||Number(r)%1===0)return 0;const[,s]=r.toString().split(".");return s?s.length:0},l=(r,e=20)=>r.length>e?`${r.slice(0,e)}...${r.slice(r.length-e,r.length)}`:r;export{l as a,i as b,n as s};
//# sourceMappingURL=formatter-DkVfX2z8.js.map
