// THE BUNDLE HALF of injected-state.html — a subresource, because a data block is by definition parsed by
// code that is not the document's own text. Both arms read the SAME two members off byte-identical records:
// one the document wrote directly, one the document shipped as a §4.12.1 data block for this file to parse.
var inl = window.__CTL_INLINE__;
fetch('/api/' + inl.props.pageProps.statusCode + '/inline-value');
if (inl.props.pageProps.user) fetch('/api/inline-gated');

var blk = JSON.parse(document.getElementById('__CTL_BLOCK__').textContent);
fetch('/api/' + blk.props.pageProps.statusCode + '/block-value');
if (blk.props.pageProps.user) fetch('/api/block-gated');
