// THE BUNDLE HALF of loaded-config.html — a subresource, because that is what a bundle is and because the
// document must not be the thing that parses the config. Every read below is off ONE record: the one the
// server answered `/cfg.json` with, for THIS visitor. A logged-in visitor is answered a different one.
fetch('/cfg.json').then(function (r) { return r.json(); }).then(function (c) {
  fetch('/api/' + c.region + '/cfg-value');   // a member the payload HOLDS: its bytes are an observation
  if (c.admin) fetch('/api/cfg-admin');       // a member it holds as `false`: unknown for the next visitor
  if (c.nested.beta) fetch('/api/cfg-beta');  // the same, one record deeper
  if (c.user) fetch('/api/cfg-user');         // a member it does NOT hold: unknown, and not `undefined`

  // AND THE RECORD ASKED TO ENUMERATE ITSELF — the four spellings state.js asks of the two in-document
  // channels, in the same order and for the same reason: they name no member, so what they ask for is the
  // record's own key list (ECMAScript §10.1.11 [[OwnPropertyKeys]] ( )). The joined list and the count ride
  // IN the address, so an enumeration that answered nothing still emits (`/api/cfg-keys-`, `/api/cfg-in-0`)
  // and is a different fact from one that never happened.
  var n = 0, k;
  fetch('/api/cfg-keys-' + Object.keys(c).join('-'));        // §20.1.2.19 -> §7.3.23 EnumerableOwnProperties
  for (k in c) n++;                                          // §14.7.5.9 EnumerateObjectProperties
  fetch('/api/cfg-in-' + n);
  fetch('/api/cfg-assign-' + Object.keys(Object.assign({}, c)).join('-'));   // §20.1.2.1 Object.assign
  fetch('/api/cfg-spread-' + Object.keys({ ...c }).join('-'));               // §7.3.25 CopyDataProperties
});
