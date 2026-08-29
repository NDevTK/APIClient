// THE BUNDLE HALF of injected-state.html — a subresource, because a data block is by definition parsed by
// code that is not the document's own text. Both arms read the SAME four members off byte-identical records:
// one the document wrote directly, one the document shipped as a §4.12.1 data block for this file to parse.
// The four are the ones cfg.js reads off the network channel's record, in the same order and for the same
// reasons, because three channels that ask different questions cannot be compared with each other.
var inl = window.__CTL_INLINE__;
fetch('/api/' + inl.props.pageProps.statusCode + '/inline-value');  // a member it HOLDS: its bytes are an observation
if (inl.admin) fetch('/api/inline-admin');                         // HELD as `false`: unknown for the next visitor
if (inl.props.pageProps.beta) fetch('/api/inline-beta');           // the same, two records deeper
if (inl.props.pageProps.user) fetch('/api/inline-gated');          // NOT held: unknown, and not `undefined`

var blk = JSON.parse(document.getElementById('__CTL_BLOCK__').textContent);
fetch('/api/' + blk.props.pageProps.statusCode + '/block-value');
if (blk.admin) fetch('/api/block-admin');
if (blk.props.pageProps.beta) fetch('/api/block-beta');
if (blk.props.pageProps.user) fetch('/api/block-gated');
