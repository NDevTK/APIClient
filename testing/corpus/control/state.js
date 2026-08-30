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

// AND THE RECORD ASKED TO ENUMERATE ITSELF — the same four spellings, in the same order, for both arms. Every
// read above names a member; these name NONE, so what they ask for is the record's own key list
// (ECMAScript §10.1.11 [[OwnPropertyKeys]] ( )). The joined list and the count are IN THE ADDRESS on purpose:
// a channel that answers the empty list still emits, at `/api/inline-keys-` and `/api/inline-in-0`, so a run
// can tell an enumeration that returned nothing from one that never happened. Averaging those two is the
// defect the whole rung exists for, one level out from the member reads above.
var iterN = 0, k;
fetch('/api/inline-keys-' + Object.keys(inl).join('-'));            // §20.1.2.19 -> §7.3.23 EnumerableOwnProperties
for (k in inl) iterN++;                                            // §14.7.5.9 EnumerateObjectProperties
fetch('/api/inline-in-' + iterN);
fetch('/api/inline-assign-' + Object.keys(Object.assign({}, inl)).join('-'));   // §20.1.2.1 Object.assign
fetch('/api/inline-spread-' + Object.keys({ ...inl }).join('-'));   // §7.3.25 CopyDataProperties

var blk = JSON.parse(document.getElementById('__CTL_BLOCK__').textContent);
fetch('/api/' + blk.props.pageProps.statusCode + '/block-value');
if (blk.admin) fetch('/api/block-admin');
if (blk.props.pageProps.beta) fetch('/api/block-beta');
if (blk.props.pageProps.user) fetch('/api/block-gated');

var blkN = 0, bk;
fetch('/api/block-keys-' + Object.keys(blk).join('-'));
for (bk in blk) blkN++;
fetch('/api/block-in-' + blkN);
fetch('/api/block-assign-' + Object.keys(Object.assign({}, blk)).join('-'));
fetch('/api/block-spread-' + Object.keys({ ...blk }).join('-'));
