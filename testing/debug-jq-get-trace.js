const fs = require('fs');
const path = require('path');
const root = path.join(__dirname, '..');
const ast = require(path.join(root, 'extension/lib/ast.js'));
const jq = fs.readFileSync(path.join(root, 'testing/harness-dumps/jquery-3.7.1.min.js'), 'utf8');
const userCode = '\njQuery.get("/api/profile");\n';
const combined = jq + userCode;
const result = ast.analyzeJSBundle(combined, 'https://example.test/test.js', { tabUrl: 'https://example.test/' });
console.log('fetchCallSites:', (result.fetchCallSites || []).length);
console.log('astLearnedMethods.length:', (result.astLearnedMethods || []).length);
if (result.astLearnedMethods) {
  result.astLearnedMethods.slice(0, 10).forEach((m, i) => {
    console.log(`  [${i}] ${m.method} ${m.url || '(no url)'}`);
  });
}
console.log('resolverErrors.length:', (result.resolverErrors || []).length);
(result.resolverErrors || []).slice(0, 5).forEach((e, i) => {
  console.log(`  resolverError[${i}]:`, e.kind, e.detail || '');
});
