// driver.js WITHOUT __feStaticSites / __feDriveStatic — only natural
// boot + event-loop pump + __hostDrive event-handler forcing. Used to
// measure how many [object Object] URLs come from __feDriveStatic
// (opaque synthetic args) vs from genuinely-opaque sources reached
// through real handlers.
(function () {
  var flush = (typeof __hostFlush === "function") ? __hostFlush : function () {};
  var drive = (typeof __hostDrive === "function") ? __hostDrive : function () {};
  var ran = (typeof __hostRan === "function") ? __hostRan : function () { return 0; };
  var pump = (typeof __hostMicrotaskDrain === "function") ? __hostMicrotaskDrain : function () { return 0; };
  var n = -1, m = ran();
  while (m !== n) { n = m; drive(); flush(); pump(); m = ran(); }
})();
