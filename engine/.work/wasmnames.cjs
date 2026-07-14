const fs = require("fs");
const path = "engine/qjs/qjs_worker.wasm";
const b = fs.readFileSync(path);
let o = 8; // magic + version
function leb() { let r = 0, s = 0, by; do { by = b[o++]; r |= (by & 0x7f) << s; s += 7; } while (by & 0x80); return r >>> 0; }
const want = new Set([42, 63, 404, 410, 575, 674, 675, 35188, 35196]);
const found = {};
let totalNamed = 0;
while (o < b.length) {
  const id = b[o++];
  const sz = leb();
  const end = o + sz;
  if (id === 0) { // custom
    const nl = leb();
    const name = b.slice(o, o + nl).toString();
    o += nl;
    if (name === "name") {
      while (o < end) {
        const sub = b[o++];
        const ssz = leb();
        const se = o + ssz;
        if (sub === 1) { // function names
          const cnt = leb();
          for (let i = 0; i < cnt; i++) {
            const idx = leb();
            const l = leb();
            const fn = b.slice(o, o + l).toString();
            o += l;
            totalNamed++;
            if (want.has(idx)) found[idx] = fn;
          }
        }
        o = se;
      }
    }
  }
  o = end;
}
console.log("namedEntries=", totalNamed);
for (const k of [42, 63, 404, 410, 575, 674, 675, 35196, 35188]) console.log(k, "->", found[k] || "(not found)");
