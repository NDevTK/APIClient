// EXTERNAL classic script: proves the engine sources and runs a subresource, not only inline text.
fetch('/api/external-top');
var region = (window.__CFG && window.__CFG.region) || 'us-east-1';
fetch('/api/' + region + '/things');
