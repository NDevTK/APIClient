require('esbuild').build({
  entryPoints: ['lib/babel-bundle-entry.js'],
  bundle: true,
  outfile: 'extension/lib/babel-bundle.js',
  format: 'iife',
  globalName: 'BabelBundle',
  platform: 'browser',
  define: { 'process.env.BABEL_TYPES_8_BREAKING': 'true' },
}).then(() => {
  console.log('Babel bundle built successfully');
}).catch((e) => {
  console.error('Build failed:', e);
  process.exit(1);
});
