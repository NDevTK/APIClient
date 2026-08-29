// THE BUNDLE HALF of xhr-config.html — a subresource, because that is what a bundle is and because the
// document must not be the thing that parses the config. The same `/cfg.json` every other control row reads,
// through the OTHER interface that hands a page a reply: XMLHttpRequest §3.6.9 The response getter's `json`
// arm, and §3.6.10 The responseText getter beneath it.
var x = new XMLHttpRequest();
x.open('GET', '/cfg.json');
x.responseType = 'json';                        // §3.6.8: the arm that parses, so `response` is the record
x.onload = function () {
  var c = x.response;                           // §3.6.9 step 7's "parse JSON from bytes"
  fetch('/api/' + c.region + '/xhr-value');     // a member the payload HOLDS: its bytes are an observation
  if (c.admin) fetch('/api/xhr-admin');         // a member it holds as `false`: unknown for the next visitor
  if (c.nested.beta) fetch('/api/xhr-beta');    // the same, one record deeper
  if (c.user) fetch('/api/xhr-user');           // a member it does NOT hold: unknown, and not `undefined`
};
x.send();

// THE SECOND ARM: the SAME reply as a STRING. It is a separate object because §3.6.10 refuses a responseType
// that is not '' or 'text', so one object cannot answer both — which is the point: the two members are two
// doors onto one server's bytes, and a run in which only one of them forks is measuring the door.
var t = new XMLHttpRequest();
t.open('GET', '/cfg.json');
t.onload = function () {
  var s = t.responseText;                       // §3.6.10, the same bytes decoded
  if (s.indexOf('"admin":true') >= 0) fetch('/api/xhr-text-admin');
};
t.send();
