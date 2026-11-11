/* The CompressionStream document: the Compression Standard §4 "Interface CompressionStream". The first shape
   is an upload helper's: a string through `new Blob([s]).stream().pipeThrough(new CompressionStream("gzip"))`,
   read whole by `new Response(stream).arrayBuffer()`. Each witness checks what the format fixes whatever blocks
   the compressor chose: RFC 1952's header bytes, CRC32 and ISIZE for "gzip", RFC 1950's CMF/FLG check and
   ADLER32 for "deflate". An unlisted format is Web IDL's TypeError, a chunk that is not a BufferSource errors
   the stream, and unknown bytes written to the writable side compress to an unknown image whose example is the
   real output.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_COMPRESS_H
#define ENGINE_HOST_FIXTURE_COMPRESS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_COMPRESS[] =
    "<!doctype html><html><body>"
    "<script>"
    "function cmzRun(f, s) {"
    "  var t = new Blob([s]).stream().pipeThrough(new CompressionStream(f));"
    "  return new Response(t).arrayBuffer().then(function (a) { return new Uint8Array(a); });"
    "}"
    "function cmzTail(b, k) { var r = []; for (var i = b.length - k; i < b.length; i++) r.push(b[i]); return r.join('.'); }"
    "try {"
    "  cmzRun('gzip', 'hello').then(function (b) {"
    "    var w = (b[0] === 31 && b[1] === 139 && b[2] === 8 && cmzTail(b, 8) === '134.166.16.54.5.0.0.0') ? 'cmzGZ' : 'cmzBAD';"
    "    fetch('/api/cmz-gzip?w=' + w);"
    "  }, function (e) { fetch('/api/cmz-gzip?w=cmzREJ'); });"
    "  cmzRun('deflate', 'hello').then(function (b) {"
    "    var w = ((b[0] & 15) === 8 && ((b[0] << 8) | b[1]) % 31 === 0 && cmzTail(b, 4) === '6.44.2.21') ? 'cmzZL' : 'cmzBAD';"
    "    fetch('/api/cmz-deflate?w=' + w);"
    "  }, function (e) { fetch('/api/cmz-deflate?w=cmzREJ'); });"
    "} catch (e) { fetch('/api/cmz-gzip?w=' + (e instanceof ReferenceError ? 'cmzREF' : 'cmzTHREW')); }"
    "var cmzF = 'cmzNONE';"
    "try { new CompressionStream('zip'); cmzF = 'cmzMADE'; } catch (e) { cmzF = e instanceof TypeError ? 'cmzTE' : 'cmzOTHER'; }"
    "fetch('/api/cmz-format?w=' + cmzF);"
    "try {"
    "  var cmzW = new CompressionStream('gzip').writable.getWriter();"
    "  cmzW.write('not bytes').then(function () { fetch('/api/cmz-chunk?w=cmzOK'); },"
    "    function (e) { fetch('/api/cmz-chunk?w=' + (e instanceof TypeError ? 'cmzTE' : 'cmzOTHER')); });"
    "} catch (e) { fetch('/api/cmz-chunk?w=cmzTHREW'); }"
    "try {"
    "  var cmzU = new TextEncoder().encode(location.hash);"
    "  var cmzS = new CompressionStream('gzip'), cmzV = cmzS.writable.getWriter();"
    "  cmzV.write(cmzU); cmzV.close();"
    "  new Response(cmzS.readable).arrayBuffer().then(function (a) {"
    "    var b = new Uint8Array(a);"
    "    fetch('/api/cmz-unk?w=' + (b[0] === 31 && b[1] === 139 ? 'cmzUNK' : 'cmzBAD'));"
    "  }, function (e) { fetch('/api/cmz-unk?w=cmzREJ'); });"
    "} catch (e) { fetch('/api/cmz-unk?w=cmzTHREW'); }"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
