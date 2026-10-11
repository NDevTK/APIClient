/* lib/url-label.js — the short label that names which page a row is about (the @S finding's source and page,
   the request log's page links, the engine-run list), shared by popup.html and dev.html. */

/* `host` and path, without a trailing slash. `host` keeps a non-default port, as the browser's own UI does: a
   port is part of the origin, so documents on two ports of one host are two origins with two frontiers and
   must not share a label. A string that does not parse as a URL is its own label. */
function _shortUrl(u) {
  try {
    var parsed = new URL(u);
    var path = parsed.pathname.replace(/\/$/, "");
    if (!path) return parsed.host;
    return parsed.host + path;
  } catch (_) {
    return u;
  }
}
