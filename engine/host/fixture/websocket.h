/* The WebSocket document: WebSockets §3 "The WebSocket interface" for the states no connection has reached,
   and the opening handshake as a request the zone answers.
   Its scripts, in order: the four constants on the interface object and its prototype; the constructor's six
   refusals; a socket's attributes and member refusals while CONNECTING; a subclass; a handshake this
   fixture's zone answers with status 200, which fails the connection (`error`, then `close` with 1006); a
   close() while CONNECTING, whose handshake is withdrawn before the zone is shown it; a socket built only
   where the absent global `wsInjected` is taken as present, whose handshake the zone refuses on that forced
   path (test_forced.c's reply arm declines /api/decl there); and a module whose top level reads
   `WebSocket.CONNECTING`, the shape a bundled noVNC has.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_WEBSOCKET_H
#define ENGINE_HOST_FIXTURE_WEBSOCKET_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_WEBSOCKET[] =
    "<!doctype html><html><body>"
    "<script>"
    "var wsC = [WebSocket.CONNECTING, WebSocket.OPEN, WebSocket.CLOSING, WebSocket.CLOSED,"
    " WebSocket.prototype.CONNECTING, WebSocket.prototype.CLOSED].join('');"
    "fetch('/api/wsconst?v=c' + wsC + (typeof WebSocket) + ('WebSocket' in window ? 'In' : 'Out'));"
    "function wsErr(f) { try { f(); return 'none'; }"
    " catch (e) { return e.name + (e instanceof DOMException ? 'D' : 'J'); } }"
    "fetch('/api/wserr?v=' + ["
    " wsErr(function () { new WebSocket('ftp://x.test/a'); }),"
    " wsErr(function () { new WebSocket('wss://x.test/a#frag'); }),"
    " wsErr(function () { new WebSocket('http://[bad'); }),"
    " wsErr(function () { new WebSocket('wss://x.test/a', ['p', 'p']); }),"
    " wsErr(function () { new WebSocket('wss://x.test/a', 'bad proto'); }),"
    " wsErr(function () { WebSocket('wss://x.test/a'); })].join('.'));"
    "var ws1 = new WebSocket('/api/ws/fail?x=1', ['chat', 'superchat']);"
    "fetch('/api/wsstate?v=' + [(ws1.url === 'wss://x.test/api/ws/fail?x=1' ? 'url' : 'URLBAD'), ws1.readyState,"
    " ws1.bufferedAmount, ws1.extensions === '', ws1.protocol === '', ws1.binaryType,"
    " wsErr(function () { ws1.send('x'); }),"
    " wsErr(function () { ws1.close(999); }),"
    " wsErr(function () { ws1.close(1000, new Array(125).join('a')); }),"
    /* `[Clamp] unsigned short`: 70000 clamps to 65535, where the modulo's 4464 would be a valid code. */
    " wsErr(function () { ws1.close(70000); }),"
    " (ws1.binaryType = 'arraybuffer', ws1.binaryType), (ws1.binaryType = 'nonsense', ws1.binaryType),"
    " ws1.onopen === null && ws1.onmessage === null && ws1.onerror === null && ws1.onclose === null,"
    " ws1 instanceof EventTarget].join('-'));"
    "class WsSub extends WebSocket { constructor(u) { super(u); this.tag = 'sub'; } }"
    "var ws4 = new WsSub('wss://x.test/api/ws/sub');"
    "fetch('/api/wssub?v=' + ws4.tag + (ws4 instanceof WsSub) + (ws4 instanceof WebSocket) + ws4.readyState);"
    "var wsLog = [];"
    "ws1.onopen = function () { wsLog.push('open'); };"
    "ws1.onerror = function () { wsLog.push('error' + ws1.readyState); };"
    "ws1.onclose = function (e) { wsLog.push('close' + ws1.readyState + 'c' + e.code + 'w' + e.wasClean + 'r'"
    " + (e.reason === '') + 'i' + (e instanceof CloseEvent)); fetch('/api/wsfail?v=' + wsLog.join('.')); };"
    "var ws2 = new WebSocket('wss://x.test/api/ws/closing'), ws2Log = [];"
    "ws2.addEventListener('error', function () { ws2Log.push('error' + ws2.readyState); });"
    "ws2.addEventListener('close', function (e) { ws2Log.push('close' + ws2.readyState + 'c' + e.code + 'w'"
    " + e.wasClean); fetch('/api/wsclosing?v=' + ws2Log.join('.') + '&b=' + ws2.bufferedAmount); });"
    "ws2.close(1000, 'bye'); ws2Log.push('state' + ws2.readyState);"
    "ws2.send('h\\u00e9llo'); ws2.send(new Uint8Array(3));"
    "if (window.wsInjected != null) { var ws3 = new WebSocket('wss://x.test/api/decl/wsforced');"
    " ws3.onclose = function (e) { fetch('/api/wsrefused?v=c' + e.code + 's' + ws3.readyState); }; }"
    "</script>"
    "<script type=module>"
    "var wsv = { CONNECTING: [WebSocket.CONNECTING, 'connecting'], CLOSED: [WebSocket.CLOSED, 'closed'] };"
    "fetch('/api/wsmodule?v=m' + wsv.CONNECTING[0] + wsv.CLOSED[0]);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
